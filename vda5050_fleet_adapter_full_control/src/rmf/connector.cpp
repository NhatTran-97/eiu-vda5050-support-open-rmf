#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <unistd.h>

#include <rclcpp/logging.hpp>

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

namespace {

// Validate the state fields required for completion, readiness, and battery
// reporting. Invalid messages are rejected without replacing the last valid state.
bool has_required_state_fields(const nlohmann::json &raw)
{
    return raw.is_object() &&
           raw.contains("orderId") && raw["orderId"].is_string() &&
           raw.contains("lastNodeId") && raw["lastNodeId"].is_string() &&
           raw.contains("driving") && raw["driving"].is_boolean() &&
           raw.contains("nodeStates") && raw["nodeStates"].is_array() &&
           raw.contains("edgeStates") && raw["edgeStates"].is_array() &&
           raw.contains("actionStates") && raw["actionStates"].is_array() &&
           raw.contains("errors") && raw["errors"].is_array() &&
           raw.contains("operatingMode") && raw["operatingMode"].is_string() &&
           raw.contains("safetyState") && raw["safetyState"].is_object() &&
           raw["safetyState"].contains("eStop") && raw["safetyState"]["eStop"].is_string() &&
           raw["safetyState"].contains("fieldViolation") &&
           raw["safetyState"]["fieldViolation"].is_boolean() &&
           raw.contains("batteryState") && raw["batteryState"].is_object() &&
           raw["batteryState"].contains("batteryCharge") &&
           raw["batteryState"]["batteryCharge"].is_number();
}

// Build a readable client ID that remains unique across hosts and containers.
std::string make_mqtt_client_id(const std::string &interface_name)
{
    char hostname[256] = {};
    if (::gethostname(hostname, sizeof(hostname) - 1) != 0)
    {
        std::snprintf(hostname, sizeof(hostname), "unknown-host");
    }

    std::random_device rd;
    std::uniform_int_distribution<std::uint32_t> dist;

    // Preserve space for the process and random uniqueness suffixes.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "_%.32s_%d_%08x", hostname, ::getpid(), dist(rd));
    return "rmf_vda5050_adapter_" + interface_name + buf;
}

}  // namespace

Connector::Connector(rclcpp::Logger logger, std::string broker_url, std::string interface_name, std::optional<std::string> username, std::optional<std::string> password)
                            : _logger(std::move(logger)),
                                _interface_name(std::move(interface_name)),
                                _mqtt_client(std::move(broker_url), make_mqtt_client_id(_interface_name), std::move(username), std::move(password))
{
    _mqtt_client.set_on_connected([this]() { on_connected(); });
    _mqtt_client.set_on_connection_lost(
        [this](const std::string &cause) { on_connection_lost(cause); });
    _mqtt_client.set_on_error(
        [this](const std::string &context, const std::string &what) { on_error(context, what); });
    _mqtt_client.set_on_message(
        [this](const std::string &topic, const std::string &payload) { handle_message(topic, payload); });
}

Connector::~Connector()
{
    shutdown();
}

void Connector::start()
{
    try
    {
        _mqtt_client.connect();
        RCLCPP_INFO(_logger, "[VDA5050] MQTT connected");
    }
    catch (const std::exception &e)
    {
        RCLCPP_ERROR(_logger, "[VDA5050] MQTT connect failed: %s", e.what());
    }
}

void Connector::shutdown()
{
    _mqtt_client.shutdown();
}

void Connector::add_robot(const std::string &name, const std::string &manufacturer,
                          const std::string &serial, const Transform &transform)
{
    auto ctx = std::make_unique<RobotContext>();
    ctx->name = name;
    ctx->manufacturer = manufacturer;
    ctx->serial = serial;
    ctx->interface_name = _interface_name;
    ctx->mqtt_needle = "/" + manufacturer + "/" + serial + "/";
    ctx->transform = transform;

    RobotContext *ctx_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ctx_ptr = ctx.get();
        _robots[name] = std::move(ctx);
    }

    subscribe_robot(*ctx_ptr);
    RCLCPP_INFO(_logger, "[VDA5050] robot '%s' -> %s/%s", name.c_str(),
                manufacturer.c_str(), serial.c_str());

    request_state(name);
}

Connector::NavigateResult Connector::navigate_route(const std::string &name,
                                                     const std::vector<RoutePoint> &route,
                                                     const std::string &map_id,
                                                     std::optional<std::size_t> released_count)
{
    if (route.empty())
    {
        RCLCPP_WARN(_logger, "[VDA5050] navigate_route: empty route for '%s'", name.c_str());
        return {};
    }

    const std::string order_id = vda5050::make_uuid();
    std::string base_id;
    std::string manufacturer, serial, interface_name;
    std::vector<vda5050::RouteWaypoint> waypoints;
    vda5050::RobotPose base{};
    int header_id = 0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] navigate_route: unknown robot '%s'", name.c_str());
            return {};
        }
        RobotContext &ctx = *it->second;

        waypoints.reserve(route.size());
        for (const auto &p : route)
        {
            // Apply the most restrictive graph and operator speed limits.
            std::optional<double> speed_limit = p.speed_limit;
            if (ctx.operator_speed_limit.has_value())
            {
                speed_limit = speed_limit.has_value()
                                  ? std::min(*speed_limit, *ctx.operator_speed_limit)
                                  : ctx.operator_speed_limit;
            }

            warn_if_unroutable(ctx, p.node_id, p.x, p.y, p.theta, map_id, speed_limit);
            const auto robot_pose = ctx.transform.to_robot(p.x, p.y, p.theta);
            waypoints.push_back(vda5050::RouteWaypoint{
                p.node_id, {robot_pose[0], robot_pose[1], robot_pose[2]}, speed_limit});
        }

        // Build the base node from the latest reported AGV state.
        base = waypoints.front().pose;
        base_id = ctx.last_node_id.empty() ? (ctx.serial + "_start") : ctx.last_node_id;
        if (ctx.last_state.has_value() && ctx.last_state->has_position())
        {
            base = {*ctx.last_state->x, *ctx.last_state->y, *ctx.last_state->theta};
        }

        header_id = ctx.next_order_header();

        // The order contains one base node and one edge per route point.
        warn_if_order_oversized(ctx, route.size() + 1, route.size());
        warn_if_map_mismatch(ctx, map_id);
        manufacturer = ctx.manufacturer;
        serial = ctx.serial;
        interface_name = ctx.interface_name;
    }

    const auto order = vda5050::build_route_order(
        header_id, order_id, manufacturer, serial, base_id, base, waypoints, map_id, 0,
        released_count);

    const std::string order_topic = vda5050::topic(interface_name, manufacturer, serial, vda5050::TOPIC_ORDER);
    const CommandStatus status = publish_raw(order_topic, order.dump());

    if (status == CommandStatus::transport_failed)
    {
        // Do not track an order that was not queued for transport.
        RCLCPP_ERROR(_logger,
                     "[VDA5050] %s -> order '%s' NOT published (transport failure) -- not "
                     "tracking it",
                     name.c_str(), order_id.c_str());
        return {CommandStatus::transport_failed, {}};
    }

    // Commit tracking only after the transport accepts the request.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it != _robots.end())
        {
            RobotContext &ctx = *it->second;
            ctx.current_order_id = order_id;
            // Track completion against the final route node.
            ctx.target_node_id = route.back().node_id;
            ctx.order_action_ids.clear();
            // A fresh orderId restarts its own update sequence.
            ctx.order_update_id = 0;
            ctx.current_route = waypoints;
            ctx.current_base_id = base_id;
            ctx.current_base = base;
            ctx.current_map_id = map_id;
            ctx.current_released_count = std::min(released_count.value_or(route.size()), route.size());
        }
    }

    RCLCPP_INFO(_logger, "[VDA5050] %s -> order '%s' over %zu waypoint(s) (%zu released), ending at '%s'",
                name.c_str(), order_id.c_str(), route.size(),
                std::min(released_count.value_or(route.size()), route.size()),
                route.back().node_id.c_str());
    return {CommandStatus::queued, order_id};
}

CommandStatus Connector::release_more(const std::string &name, std::size_t released_count)
{
    std::string order_id, base_id, manufacturer, serial, interface_name, map_id;
    std::vector<vda5050::RouteWaypoint> waypoints;
    vda5050::RobotPose base{};
    int header_id = 0;
    int order_update_id = 0;
    std::size_t clamped = 0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] release_more: unknown robot '%s'", name.c_str());
            return CommandStatus::transport_failed;
        }
        RobotContext &ctx = *it->second;

        clamped = std::min(released_count, ctx.current_route.size());
        if (ctx.current_order_id.empty() || ctx.current_route.empty() ||
            clamped <= ctx.current_released_count)
        {
            // Nothing to extend, or no growth past what's already released.
            return CommandStatus::transport_failed;
        }

        order_id = ctx.current_order_id;
        order_update_id = ++ctx.order_update_id;
        waypoints = ctx.current_route;
        base_id = ctx.current_base_id;
        base = ctx.current_base;
        map_id = ctx.current_map_id;
        header_id = ctx.next_order_header();
        manufacturer = ctx.manufacturer;
        serial = ctx.serial;
        interface_name = ctx.interface_name;
    }

    const auto order = vda5050::build_route_order(
        header_id, order_id, manufacturer, serial, base_id, base, waypoints, map_id,
        order_update_id, clamped);

    const std::string order_topic =
        vda5050::topic(interface_name, manufacturer, serial, vda5050::TOPIC_ORDER);
    const CommandStatus status = publish_raw(order_topic, order.dump());

    if (status == CommandStatus::transport_failed)
    {
        // The orderUpdateId counter is not rolled back: VDA5050 only
        // requires it to increase, not to be gap-free, and the next
        // successful release_more() will simply use the next value.
        RCLCPP_ERROR(_logger,
                     "[VDA5050] %s -> order '%s' update %d NOT published (transport failure)",
                     name.c_str(), order_id.c_str(), order_update_id);
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it != _robots.end())
        {
            it->second->current_released_count = clamped;
        }
    }

    RCLCPP_INFO(_logger,
                "[VDA5050] %s -> order '%s' update %d: released %zu/%zu route point(s)",
                name.c_str(), order_id.c_str(), order_update_id, clamped, waypoints.size());
    return status;
}

bool Connector::set_speed_limit(const std::string &name, std::optional<double> limit)
{
    if (limit.has_value() && (!std::isfinite(*limit) || *limit <= 0.0))
    {
        RCLCPP_ERROR(_logger,
                     "[VDA5050] %s: speed limit %.3f is not a usable speed -- pass no "
                     "limit to clear the cap instead",
                     name.c_str(), *limit);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] set_speed_limit: unknown robot '%s'",
                         name.c_str());
            return false;
        }
        it->second->operator_speed_limit = limit;
    }

    if (limit.has_value())
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s: operator speed limit set to %.2f m/s -- applies to the "
                    "next order, not the one already being driven", name.c_str(), *limit);
    }
    else
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s: operator speed limit cleared -- the next order carries " "only the nav graph's own lane limits",  name.c_str());
    }
    return true;
}

std::optional<double> Connector::speed_limit(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
        return std::nullopt;
    }
    return it->second->operator_speed_limit;
}

CommandStatus Connector::stop(const std::string &name)
{
    std::string topic;
    nlohmann::json msg;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return CommandStatus::transport_failed;
        }
        RobotContext &ctx = *it->second;

        msg = vda5050::build_cancel_order(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, blocking_type_for(ctx, "cancelOrder", "HARD"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);
    }

    const CommandStatus status = publish_raw(topic, msg.dump());
    if (status == CommandStatus::transport_failed)
    {
        // Preserve active-order tracking when cancelOrder is not queued.
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> cancelOrder NOT published (transport failure) -- " "still tracking the order as active", name.c_str());
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it != _robots.end())
        {
            RobotContext &ctx = *it->second;
            ctx.current_order_id.clear();
            ctx.target_node_id.clear();
            ctx.order_action_ids.clear();
        }
    }
    RCLCPP_INFO(_logger, "[VDA5050] %s -> cancelOrder", name.c_str());
    return status;
}

std::string Connector::init_position(const std::string &name, double x, double y,
                                     double theta, const std::string &map_id)
{
    std::string topic;
    vda5050::InstantActionRequest request;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] init_position: unknown robot '%s'", name.c_str());
            return {};
        }
        RobotContext &ctx = *it->second;

        const auto robot_pose = ctx.transform.to_robot(x, y, theta);

        const nlohmann::json params{
            {"x", robot_pose[0]},
            {"y", robot_pose[1]},
            {"theta", robot_pose[2]},
            {"mapId", map_id},
        };

        request = vda5050::build_instant_action(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, "initPosition", params, blocking_type_for(ctx, "initPosition", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);

        if (!ctx.current_order_id.empty())
        {
            RCLCPP_WARN(_logger,"[VDA5050] %s: sending initPosition while order '%s' is still "
                        "tracked -- AGVs commonly refuse to re-localize mid-order",
                        name.c_str(), ctx.current_order_id.c_str());
        }
    }
    if (publish_raw(topic, request.message.dump()) == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> initPosition NOT published (transport failure)",
                     name.c_str());
        return {};
    }
    RCLCPP_INFO(_logger, "[VDA5050] %s -> initPosition (%.2f, %.2f, %.2f) on '%s'", name.c_str(), x, y, theta, map_id.c_str());
    return request.action_id;
}

CommandStatus Connector::pause(const std::string &name)
{
    std::string topic;
    nlohmann::json msg;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return CommandStatus::transport_failed;
        }
        RobotContext &ctx = *it->second;

        msg = vda5050::build_start_pause(ctx.next_instant_actions_header(),
                                         ctx.manufacturer, ctx.serial,
                                         blocking_type_for(ctx, "startPause", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);
    }
    const CommandStatus status = publish_raw(topic, msg.dump());
    if (status == CommandStatus::queued)
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s -> startPause", name.c_str());
    }
    else
    {
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> startPause NOT published (transport failure)",
                     name.c_str());
    }
    return status;
}

CommandStatus Connector::resume(const std::string &name)
{
    std::string topic;
    nlohmann::json msg;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return CommandStatus::transport_failed;
        }
        RobotContext &ctx = *it->second;

        msg = vda5050::build_stop_pause(ctx.next_instant_actions_header(),  ctx.manufacturer, ctx.serial, blocking_type_for(ctx, "stopPause", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);
    }
    const CommandStatus status = publish_raw(topic, msg.dump());
    if (status == CommandStatus::queued)
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s -> stopPause", name.c_str());
    }
    else
    {
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> stopPause NOT published (transport failure)", name.c_str());
    }
    return status;
}

std::string Connector::execute_instant_action(
    const std::string &name, const std::string &action_type,
    const nlohmann::json &parameters)
{
    std::string topic;
    vda5050::InstantActionRequest request;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return {};
        }
        RobotContext &ctx = *it->second;

        if (ctx.factsheet.has_value() && !ctx.factsheet->supports_action(action_type))
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: action '%s' is not in the AGV's factsheet " "(protocolFeatures.agvActions) -- sending it anyway",
                        name.c_str(), action_type.c_str());
        }
        else if (ctx.factsheet.has_value() && !ctx.factsheet->supports_scope(action_type, "INSTANT"))
        {
            // This API publishes only INSTANT-scoped actions.
            RCLCPP_WARN(_logger,"[VDA5050] %s: action '%s' is scoped NODE/EDGE only in the AGV's "
                        "factsheet, not INSTANT -- sending it as an instantAction anyway",
                        name.c_str(), action_type.c_str());
        }

        const std::string blocking_type = blocking_type_for(ctx, action_type, "HARD");
        warn_if_action_conflicts(ctx, action_type, blocking_type);

        request = vda5050::build_instant_action(ctx.next_instant_actions_header(),
                                                ctx.manufacturer, ctx.serial, action_type, parameters, blocking_type);
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);
    }
    if (publish_raw(topic, request.message.dump()) == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger,"[VDA5050] %s -> action '%s' NOT published (transport failure)", name.c_str(), action_type.c_str());
        return {};
    }
    return request.action_id;
}

void Connector::subscribe_robot(const RobotContext &ctx)
{
    for (const char *leaf :
         {
            vda5050::TOPIC_STATE, vda5050::TOPIC_CONNECTION, vda5050::TOPIC_VISUALIZATION, vda5050::TOPIC_FACTSHEET
        })
    {
        _mqtt_client.subscribe(vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, leaf), 1);
    }
}

Connector::RobotContext *Connector::match_robot(const std::string &topic)
{
    for (auto &[_, ctx] : _robots)
    {
        if (topic.find(ctx->mqtt_needle) != std::string::npos)
        {
            return ctx.get();
        }
    }
    return nullptr;
}

void Connector::warn_if_unroutable(const RobotContext &ctx, const std::string &dest_node_id,
                                   double x, double y, double theta, const std::string &map_id, std::optional<double> speed_limit) const
{
    if (dest_node_id.empty())
    {
        RCLCPP_WARN(_logger,"[VDA5050] %s: navigating to an empty nodeId -- the AGV cannot "
                    "echo it back as lastNodeId, so this order can never complete", ctx.name.c_str());
    }
    if (map_id.empty())
    {
        RCLCPP_WARN(_logger, "[VDA5050] %s: navigating with an empty mapId", ctx.name.c_str());
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(theta))
    {
        RCLCPP_WARN(_logger, "[VDA5050] %s: destination is not finite (%.3f, %.3f, %.3f)", ctx.name.c_str(), x, y, theta);
    }

    if (speed_limit.has_value())
    {
        if (!std::isfinite(*speed_limit) || *speed_limit <= 0.0)
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: speed limit %.3f is not a usable speed", ctx.name.c_str(), *speed_limit);
        }
        else if (ctx.factsheet.has_value() && ctx.factsheet->speed_max.has_value() && *speed_limit > *ctx.factsheet->speed_max)
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: speed limit %.3f exceeds the AGV's declared " "speedMax %.3f (factsheet)", ctx.name.c_str(), *speed_limit, *ctx.factsheet->speed_max);
        }
    }
}

void Connector::warn_if_action_conflicts(const RobotContext &ctx,  const std::string &action_type, const std::string &blocking_type) const
{
    if (!ctx.last_state.has_value())
    {
        return;
    }
    const auto &s = *ctx.last_state;

    // HARD and SOFT actions require motion to stop; NONE may run in parallel.
    if (s.driving && blocking_type != "NONE")
    {
        RCLCPP_WARN(_logger,
                    "[VDA5050] %s: sending '%s' (blockingType %s) while the AGV is still "
                    "driving -- %s actions expect it stationary and may be queued or "
                    "rejected",
                    ctx.name.c_str(), action_type.c_str(), blocking_type.c_str(),
                    blocking_type.c_str());
    }

    if (!ctx.factsheet.has_value())
    {
        return;
    }

    // A running HARD-only action holds exclusive execution.
    for (const auto &running : s.action_states)
    {
        const std::string status = running.value("actionStatus", std::string{});
        // Ignore action states that do not provide a status.
        if (status.empty() || vda5050::is_terminal_action_status(status))
        {
            continue;
        }
        const std::string running_type = running.value("actionType", std::string{});
        if (running_type.empty() || running_type == action_type)
        {
            continue;
        }
        // Report only conflicts whose blocking type is unambiguous.
        const auto it = ctx.factsheet->agv_actions.find(running_type);
        if (it != ctx.factsheet->agv_actions.end() &&
            it->second.blocking_types.size() == 1 &&
            it->second.blocking_types.front() == "HARD")
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: sending '%s' while '%s' (action %s) is still %s "
                        "and only ever runs HARD -- it holds exclusivity, this action "
                        "may be queued or rejected",
                        ctx.name.c_str(), action_type.c_str(), running_type.c_str(),
                        running.value("actionId", std::string{}).c_str(), status.c_str());
        }
    }
}

void Connector::warn_if_order_oversized(RobotContext &ctx, std::size_t node_count,
                                        std::size_t edge_count) const
{
    if (ctx.factsheet.has_value())
    {
        const auto &fs = *ctx.factsheet;
        if (fs.max_order_nodes.has_value() && node_count > *fs.max_order_nodes)
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: order has %zu node(s), exceeding the AGV's "
                        "declared protocolLimits.maxArrayLens['order.nodes'] (%u) -- it "
                        "may reject this order",
                        ctx.name.c_str(), node_count, *fs.max_order_nodes);
        }
        if (fs.max_order_edges.has_value() && edge_count > *fs.max_order_edges)
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: order has %zu edge(s), exceeding the AGV's "
                        "declared protocolLimits.maxArrayLens['order.edges'] (%u) -- it "
                        "may reject this order",
                        ctx.name.c_str(), edge_count, *fs.max_order_edges);
        }

        if (fs.min_order_interval.has_value())
        {
            const double since_last = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - ctx.last_order_time).count();
            if (since_last < *fs.min_order_interval)
            {
                RCLCPP_WARN(_logger,
                            "[VDA5050] %s: order published %.2fs after the previous one, "
                            "under the AGV's declared minOrderInterval (%.2fs)",
                            ctx.name.c_str(), since_last, *fs.min_order_interval);
            }
        }
    }
    ctx.last_order_time = std::chrono::steady_clock::now();
}

void Connector::warn_if_map_mismatch(const RobotContext &ctx, const std::string &order_map_id) const
{
    if (order_map_id.empty() || !ctx.last_state.has_value() || ctx.last_state->map_id.empty())
    {
        return;
    }
    if (ctx.last_state->map_id != order_map_id)
    {
        RCLCPP_WARN(_logger,
                    "[VDA5050] %s: order mapId '%s' does not match the AGV's own "
                    "reported mapId '%s' -- RMF's nav graph and the robot's map "
                    "config may have drifted apart",
                    ctx.name.c_str(), order_map_id.c_str(), ctx.last_state->map_id.c_str());
    }
}

std::string Connector::blocking_type_for(const RobotContext &ctx,
                                         const std::string &action_type,
                                         const std::string &preferred)
{
    if (!ctx.factsheet.has_value())
    {
        return preferred;
    }
    return ctx.factsheet->blocking_type_for(action_type, preferred);
}

CommandStatus Connector::publish_raw(const std::string &topic, const std::string &payload)
{
    if (!_mqtt_client.publish(topic, payload))
    {
        // Detailed transport failures are reported through on_error().
        RCLCPP_WARN(_logger, "[VDA5050] publish failed, dropping message to %s", topic.c_str());
        return CommandStatus::transport_failed;
    }
    return CommandStatus::queued;
}

void Connector::request_state(const std::string &name)
{
    std::string topic;
    nlohmann::json msg;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return;
        }
        RobotContext &ctx = *it->second;
        msg = vda5050::build_state_request(ctx.next_instant_actions_header(),
                                           ctx.manufacturer, ctx.serial);
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);
    }
    publish_raw(topic, msg.dump());
}

void Connector::on_connected()
{
    RCLCPP_INFO(_logger, "[VDA5050] MQTT (re)connected");
}

void Connector::on_connection_lost(const std::string &cause)
{
    RCLCPP_WARN(_logger, "[VDA5050] MQTT connection lost: %s", cause.c_str());
}

void Connector::on_error(const std::string &context, const std::string &what)
{
    RCLCPP_WARN(_logger, "[VDA5050] %s: %s", context.c_str(), what.c_str());
}

void Connector::report_state_changes(RobotContext &ctx)
{
    const auto &s = *ctx.last_state;

    // Report changes in errors and pause state.
    std::string errors_key;
    for (const auto &e : s.errors)
    {
        errors_key += e.dump() + ";";
    }
    if (s.paused)
    {
        errors_key += "paused;";
    }
    if (errors_key != ctx.last_errors_key)
    {
        if (errors_key.empty())
        {
            RCLCPP_INFO(_logger, "[VDA5050] %s: errors cleared", ctx.name.c_str());
        }
        else
        {
            RCLCPP_ERROR(_logger, "[VDA5050] %s reports: %s", ctx.name.c_str(),
                         errors_key.c_str());
        }
        ctx.last_errors_key = errors_key;
    }

    // Report changes in safety state.
    const std::string safety_key = s.safety_state.e_stop + (s.safety_state.field_violation ? "|field" : "");
    if (safety_key != ctx.last_safety_key)
    {
        if (s.safety_state.triggered())
        {
            RCLCPP_ERROR(_logger,
                         "[VDA5050] %s SAFETY: eStop '%s'%s -- the AGV will not drive",
                         ctx.name.c_str(), s.safety_state.e_stop.c_str(),
                         s.safety_state.field_violation ? ", protective field violated" : "");
        }
        else if (!ctx.last_safety_key.empty())
        {
            RCLCPP_INFO(_logger, "[VDA5050] %s safety cleared", ctx.name.c_str());
        }
        ctx.last_safety_key = safety_key;
    }

    // Report changes in operating mode.
    if (s.operating_mode != ctx.last_mode_key)
    {
        if (s.operable())
        {
            RCLCPP_INFO(_logger, "[VDA5050] %s operating mode: %s", ctx.name.c_str(),
                        s.operating_mode.c_str());
        }
        else
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s operating mode: %s -- under local control, it will "
                        "not act on orders from this fleet adapter",
                        ctx.name.c_str(), s.operating_mode.c_str());
        }
        ctx.last_mode_key = s.operating_mode;
    }

    if (s.new_base_request != ctx.last_new_base_request)
    {
        if (s.new_base_request)
        {
            // Purely diagnostic -- release is driven by Plan::Waypoint::time() (see honor_waypoint_timing()), never by this request.
            const std::size_t total = ctx.current_route.size();
            if (ctx.current_released_count >= total)
            {
                RCLCPP_WARN(_logger,
                            "[VDA5050] %s requests a new base but order '%s' has no more "
                            "route points to release (%zu/%zu already released) -- the AGV's "
                            "own base tracking may have diverged from this adapter's",
                            ctx.name.c_str(),
                            ctx.current_order_id.empty() ? "(none)" : ctx.current_order_id.c_str(),
                            ctx.current_released_count, total);
            }
            else
            {
                RCLCPP_INFO(_logger,
                            "[VDA5050] %s requests a new base -- waiting at the release "
                            "boundary of order '%s' (%zu/%zu route point(s) released)",
                            ctx.name.c_str(),
                            ctx.current_order_id.empty() ? "(none)" : ctx.current_order_id.c_str(),
                            ctx.current_released_count, total);
            }
        }
        ctx.last_new_base_request = s.new_base_request;
    }

    // Report VDA5050 informational messages.
    std::string info_key;
    for (const auto &i : s.information)
    {
        info_key += i.dump() + ";";
    }
    if (info_key != ctx.last_info_key)
    {
        if (!info_key.empty())
        {
            RCLCPP_INFO(_logger, "[VDA5050] %s info: %s", ctx.name.c_str(), info_key.c_str());
        }
        ctx.last_info_key = info_key;
    }

    // Report the current load set.
    std::string loads_key;
    for (const auto &l : s.loads)
    {
        loads_key += l.value("loadId", std::string{"?"}) + "(" + l.value("loadType", std::string{"?"}) + ");";
    }
    if (loads_key != ctx.last_loads_key)
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s loads: %s", ctx.name.c_str(),loads_key.empty() ? "(empty)" : loads_key.c_str());
        ctx.last_loads_key = loads_key;
    }

    // Report maps declared by the AGV.
    std::string maps_key;
    for (const auto &m : s.maps)
    {
        maps_key += m.value("mapId", std::string{"?"}) + ":" + m.value("mapStatus", std::string{"?"}) + ";";
    }
    if (maps_key != ctx.last_maps_key)
    {
        if (!maps_key.empty())
        {
            RCLCPP_INFO(_logger, "[VDA5050] %s maps: %s", ctx.name.c_str(), maps_key.c_str());
        }
        ctx.last_maps_key = maps_key;
    }
}

void Connector::handle_message(const std::string &topic, const std::string &payload)
{
    nlohmann::json raw;
    try
    {
        raw = nlohmann::json::parse(payload);
    }
    catch (const std::exception &)
    {
        RCLCPP_WARN(_logger, "[VDA5050] bad payload on %s", topic.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    RobotContext *ctx = match_robot(topic);
    if (!ctx)
    {
        return;
    }

    auto ends_with = [&](const char *leaf)
    {
        const std::string s = leaf;
        return topic.size() >= s.size() && topic.compare(topic.size() - s.size(), s.size(), s) == 0;
    };

    if (ends_with(vda5050::TOPIC_STATE))
    {
        if (!has_required_state_fields(raw))
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: state message missing/mistyping a VDA5050-required "
                        "field (orderId/lastNodeId/driving/nodeStates/edgeStates/"
                        "actionStates/errors/operatingMode/safetyState.{eStop,fieldViolation}/"
                        "batteryState.batteryCharge) "
                        "-- rejecting rather than caching it with fallback values",
                        ctx->name.c_str());
            return;
        }
        ctx->last_state = vda5050::ParsedState(raw);
        ctx->last_state_time = std::chrono::steady_clock::now();
        if (!ctx->last_state->last_node_id.empty())
        {
            ctx->last_node_id = ctx->last_state->last_node_id;
        }

        report_state_changes(*ctx);
    }
    else if (ends_with(vda5050::TOPIC_VISUALIZATION))
    {
        vda5050::ParsedVisualization viz(raw);
        // Ignore visualization messages without a usable pose.
        if (!viz.has_position())
        {
            return;
        }
        ctx->last_visualization = std::move(viz);
        ctx->last_visualization_time = std::chrono::steady_clock::now();
    }
    else if (ends_with(vda5050::TOPIC_CONNECTION))
    {
        const std::string conn = raw.value("connectionState", std::string{});
        const bool online = (conn == "ONLINE");
        if (ctx->connected != online)
        {
            if (online)
            {
                RCLCPP_INFO(_logger, "[VDA5050] %s ONLINE", ctx->name.c_str());
            }
            else if (ctx->connected == true)
            {
                RCLCPP_WARN(_logger, "[VDA5050] %s %s", ctx->name.c_str(), conn.empty() ? "OFFLINE" : conn.c_str());
            }
        }
        ctx->connected = online;
    }
    else if (ends_with(vda5050::TOPIC_FACTSHEET))
    {
        vda5050::ParsedFactsheet fs(raw);
        if (!fs.has_content())
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: factsheet carried nothing usable, ignoring", ctx->name.c_str());
            return;
        }

        std::string actions;
        for (const auto &[type, info] : fs.agv_actions)
        {
            actions += actions.empty() ? "" : ", ";
            actions += type;
            if (!info.blocking_types.empty())
            {
                actions += "(";
                for (std::size_t i = 0; i < info.blocking_types.size(); ++i)
                {
                    actions += (i == 0 ? "" : "|") + info.blocking_types[i];
                }
                actions += ")";
            }
        }

        RCLCPP_INFO(_logger,
                    "[VDA5050] %s factsheet: series '%s', %s/%s, speedMax %.2f, actions: %s", ctx->name.c_str(), fs.series_name.c_str(), fs.agv_kinematic.c_str(),
                    fs.agv_class.c_str(), fs.speed_max.value_or(0.0), actions.empty() ? "(none declared)" : actions.c_str());

        ctx->factsheet = std::move(fs);
    }
}

std::optional<RobotData> Connector::get_data(const std::string &name)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
        return std::nullopt;
    }
    const RobotContext &ctx = *it->second;
    if (!ctx.last_state.has_value() || !ctx.last_state->has_position())
    {
        return std::nullopt;
    }

    const auto &s = *ctx.last_state;

    RobotData data;
    data.map_name = s.map_id;
    data.position = ctx.transform.to_rmf(*s.x, *s.y, *s.theta);
    data.battery_soc = std::clamp(s.battery_soc.value_or(1.0), 0.0, 1.0);
    data.charging = s.charging;
    data.order_id = s.order_id;
    data.last_node_id = ctx.last_node_id;
    data.last_node_sequence_id = s.last_node_sequence_id;
    data.velocity = s.velocity;
    data.distance_since_last_node = s.distance_since_last_node;
    data.operating_mode = s.operating_mode;
    data.operable = s.operable();
    data.safety_state = s.safety_state;
    data.fatal_error = s.first_fatal_error();
    data.paused = s.paused;
    data.new_base_request = s.new_base_request;
    data.localization_score = s.localization_score;

    // Prefer fresher visualization pose and velocity over state telemetry.
    if (ctx.last_visualization.has_value() && ctx.last_visualization_time > ctx.last_state_time)
    {
        const auto &v = *ctx.last_visualization;
        // Do not combine telemetry from different map frames.
        if (v.map_id.empty() || v.map_id == s.map_id)
        {
            data.position = ctx.transform.to_rmf(*v.x, *v.y, *v.theta);
            if (v.velocity.has_value())
            {
                data.velocity = v.velocity;
            }
        }
    }

    return data;
}

bool Connector::is_command_completed(const std::string &name)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
        return false;
    }

    RobotContext &ctx = *it->second;
    if (!ctx.last_state.has_value() || ctx.current_order_id.empty())
    {
        return false;
    }

    const auto &s = *ctx.last_state;

    if (s.order_id.empty() || s.order_id != ctx.current_order_id)
    {
        return false;
    }
    if (s.driving)
    {
        return false;
    }

    if (s.order_finished(ctx.current_order_id, ctx.target_node_id, ctx.order_action_ids))
    {
        return true;
    }

    // Report order actions that are still active after navigation settles.
    if (s.node_states.empty() && s.edge_states.empty() && !s.driving &&
        !s.actions_settled(ctx.order_action_ids))
    {
        std::string pending;
        for (const auto &id : ctx.order_action_ids)
        {
            const auto status = s.action_status(id);
            if (status.has_value() && !vda5050::is_terminal_action_status(*status))
            {
                pending += pending.empty() ? "" : ", ";
                pending += id + "=" + *status;
            }
        }
        const std::string key = ctx.current_order_id + "|actions|" + pending;
        if (ctx.last_incomplete_key != key)
        {
            ctx.last_incomplete_key = key;
            RCLCPP_INFO(_logger,"[VDA5050] %s reached the end of order '%s'; waiting on action(s): %s",
                            name.c_str(), ctx.current_order_id.c_str(), pending.c_str());
        }
        return false;
    }

    // Report a final-node mismatch after the order has drained.
    if (s.node_states.empty() && s.edge_states.empty() && !ctx.target_node_id.empty() && s.last_node_id != ctx.target_node_id)
    {
        const std::string key = ctx.current_order_id + "|" + s.last_node_id;
        if (ctx.last_incomplete_key != key)
        {
            ctx.last_incomplete_key = key;
            RCLCPP_WARN(_logger,"[VDA5050] %s drained order '%s' but reports lastNodeId '%s' "
                                "while the order targeted '%s'. Navigation cannot complete "
                                "until the robot echoes the nodeId this adapter sends.",
                                name.c_str(), ctx.current_order_id.c_str(),
                                s.last_node_id.empty() ? "(empty)" : s.last_node_id.c_str(), ctx.target_node_id.c_str());
        }
    }

    return false;
}

bool Connector::is_order_stuck(const std::string &name, double timeout_s) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
        return false;
    }

    const RobotContext &ctx = *it->second;
    if (ctx.current_order_id.empty())
    {
        return false;
    }

    const std::string reported_order_id = ctx.last_state.has_value() ? ctx.last_state->order_id : std::string{};
    if (reported_order_id == ctx.current_order_id)
    {
        return false;
    }

    const double since_dispatch = std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx.last_order_time).count();
    return since_dispatch > timeout_s;
}

std::optional<std::string> Connector::get_action_state(
    const std::string &name, const std::string &action_id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end() || !it->second->last_state.has_value())
    {
        return std::nullopt;
    }
    for (const auto &a : it->second->last_state->action_states)
    {
        if (a.value("actionId", std::string{}) == action_id)
        {
            return a.value("actionStatus", std::string{});
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> Connector::get_action_result(
    const std::string &name, const std::string &action_id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end() || !it->second->last_state.has_value())
    {
        return std::nullopt;
    }
    for (const auto &a : it->second->last_state->action_states)
    {
        if (a.value("actionId", std::string{}) == action_id)
        {
            return std::make_pair(a.value("actionStatus", std::string{}), a.value("resultDescription", std::string{}));
        }
    }
    return std::nullopt;
}

std::optional<std::string> Connector::get_known_map(const std::string &name)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end() || !it->second->last_state.has_value())
    {
        return std::nullopt;
    }
    const RobotContext &ctx = *it->second;
    if (!ctx.last_state->map_id.empty())
    {
        return ctx.last_state->map_id;
    }
    return ctx.current_map_id;
}

bool Connector::is_online(const std::string &name, double state_timeout_s)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
        return false;
    }
    const RobotContext &ctx = *it->second;
    if (ctx.connected == false)
    {
        return false;
    }
    if (!ctx.last_state.has_value())
    {
        return false;
    }
    const auto age = std::chrono::duration<double>( std::chrono::steady_clock::now() - ctx.last_state_time).count();
    return age <= state_timeout_s;
}

}  // namespace vda5050_fleet_adapter_full_control::rmf
