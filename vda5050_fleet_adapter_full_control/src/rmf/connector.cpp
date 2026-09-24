#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <unistd.h>

#include <rclcpp/logging.hpp>

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/json_read.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_validation.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/route_stitch.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

namespace {

// Shortest time between two log lines about the same problem that repeats with every message.
constexpr std::chrono::seconds kRepeatedLogInterval{30};

// Suffix that says how many similar messages a log line stands for.
std::string suppressed_note(std::size_t held)
{
    return held == 0 ? std::string{} : " (" + std::to_string(held) + " similar message(s) suppressed)";
}

// Validate state fields needed for readiness, progress, and battery reporting.
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

// Create a readable MQTT client ID that is unique across hosts and containers.
std::string make_mqtt_client_id(const std::string &interface_name)
{
    char hostname[256] = {};
    if (::gethostname(hostname, sizeof(hostname) - 1) != 0)
    {
        std::snprintf(hostname, sizeof(hostname), "unknown-host");
    }

    std::random_device rd;
    std::uniform_int_distribution<std::uint32_t> dist;

    // Reserve room for the process and random ID suffixes.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "_%.32s_%d_%08x", hostname, ::getpid(), dist(rd));
    return "rmf_vda5050_adapter_" + interface_name + buf;
}

// Split "<interface>/v2/<manufacturer>/<serial>/<leaf>" into its five levels.
std::vector<std::string> split_topic(const std::string &topic)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const auto slash = topic.find('/', start);
        if (slash == std::string::npos)
        {
            parts.push_back(topic.substr(start));
            return parts;
        }
        parts.push_back(topic.substr(start, slash - start));
        start = slash + 1;
    }
}

}  // namespace

Connector::Connector(const rclcpp::Logger &logger, const std::string &broker_url, std::string interface_name, std::optional<std::string> username, std::optional<std::string> password,
                     const mqtt::MqttOptions &mqtt_options)
                            : _logger(logger),
                                _interface_name(std::move(interface_name)),
                                _tls(mqtt_options.tls),
                                _has_credentials(username.has_value() || password.has_value()),
                                _mqtt_client(broker_url, make_mqtt_client_id(_interface_name), std::move(username), std::move(password), mqtt_options),
                                _repeat_log(kRepeatedLogInterval)
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
    if (_tls.enabled)
    {
        RCLCPP_INFO(_logger, "[VDA5050] MQTT over TLS (CA %s, client certificate %s, hostname check %s)",
                    _tls.ca_file.empty() ? "from the system" : _tls.ca_file.c_str(),
                    _tls.client_cert.empty() ? "none" : _tls.client_cert.c_str(), _tls.verify_hostname ? "on" : "off");
        if (!_tls.verify_hostname)
        {
            RCLCPP_WARN(_logger, "[VDA5050] the broker's certificate is not checked against its host name");
        }
    }
    else if (_has_credentials)
    {
        RCLCPP_WARN(_logger, "[VDA5050] MQTT credentials are sent without TLS; set vda5050.mqtt.tls.enabled");
    }

    // Learn about robots that are not registered yet from their connection messages.
    _mqtt_client.subscribe(vda5050::topic(_interface_name, "+", "+", vda5050::TOPIC_CONNECTION), 1);
    try
    {
        _mqtt_client.connect();
        RCLCPP_INFO(_logger, "[VDA5050] MQTT connecting");
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
    for (const std::string *part : {&manufacturer, &serial})
    {
        if (part->empty() || part->find_first_of("/+#") != std::string::npos)
        {
            throw std::invalid_argument("robot '" + name + "': manufacturer and serial must not be empty or contain '/', '+' or '#'");
        }
    }

    auto ctx = std::make_shared<RobotContext>();
    ctx->name = name;
    ctx->manufacturer = manufacturer;
    ctx->serial = serial;
    ctx->interface_name = _interface_name;
    ctx->transform = transform;

    const RobotContext *ctx_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ctx_ptr = ctx.get();
        _robot_index[manufacturer + "/" + serial] = ctx;
        _robots[name] = std::move(ctx);
        _discovered.erase(manufacturer + "/" + serial);
    }

    subscribe_robot(*ctx_ptr);
    RCLCPP_INFO(_logger, "[VDA5050] robot '%s' -> %s/%s", name.c_str(),
                manufacturer.c_str(), serial.c_str());

    // Request a state when already connected.
    if (_mqtt_client.is_connected())
    {
        request_state(name);
    }
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

    const auto order_lock = lock_orders(name);

    const std::string order_id = vda5050::make_uuid();
    std::string base_id;
    std::string manufacturer, serial, interface_name;
    std::vector<vda5050::RouteWaypoint> waypoints;
    vda5050::RobotPose base{};
    vda5050::NodeDeviation deviation;
    int header_id = 0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] navigate_route: unknown robot '%s'", name.c_str());
            return {};
        }
        deviation = _node_deviation;
        RobotContext &ctx = *it->second;

        waypoints = to_waypoints(ctx, route, map_id);

        // Build the base node from the latest reported AGV state.
        base = waypoints.front().pose;
        base_id = ctx.last_node_id.empty() ? (ctx.serial + "_start") : ctx.last_node_id;
        if (ctx.last_state.has_value() && ctx.last_state->has_position())
        {
            base = {*ctx.last_state->x, *ctx.last_state->y, *ctx.last_state->theta};
        }

        header_id = ctx.next_order_header();

        // The order contains one base node and one edge per route point.
        warn_if_map_mismatch(ctx, map_id);
        if (!order_allowed(ctx, base, waypoints, map_id, route.size() + 1, route.size()))
        {
            RCLCPP_ERROR(_logger, "[VDA5050] %s: order '%s' NOT sent -- it violates the AGV's declared limits", name.c_str(), order_id.c_str());
            return {CommandStatus::rejected, {}};
        }
        manufacturer = ctx.manufacturer;
        serial = ctx.serial;
        interface_name = ctx.interface_name;
    }

    const auto order = vda5050::build_route_order(
        header_id, order_id, manufacturer, serial, base_id, base, waypoints, map_id, 0, released_count, 0, deviation);

    const std::string order_topic = vda5050::topic(interface_name, manufacturer, serial, vda5050::TOPIC_ORDER);
    const CommandStatus status = publish_raw(order_topic, order.dump());

    if (status == CommandStatus::transport_failed)
    {
        // Leave order tracking unchanged if MQTT cannot queue the message.
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> order '%s' NOT published (transport failure) -- not " "tracking it", name.c_str(), order_id.c_str());
        return {CommandStatus::transport_failed, {}};
    }

    // Track the order only after MQTT accepts the publish request.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it != _robots.end())
        {
            RobotContext &ctx = *it->second;
            ctx.current_order_id = order_id;
            ctx.order_done = false;
            ctx.cancel.clear();
            // Track completion against the final route node.
            ctx.target_node_id = route.back().node_id;
            ctx.order_action_ids.clear();
            // A fresh orderId restarts its own update sequence.
            ctx.order_update_id = 0;
            ctx.route_offset = 0;
            ctx.current_route = waypoints;
            ctx.current_base_id = base_id;
            ctx.current_base = base;
            ctx.current_map_id = map_id;
            ctx.current_released_count = std::min(released_count.value_or(route.size()), route.size());
        }
    }

    RCLCPP_INFO(_logger, "[VDA5050] %s -> order '%s' over %zu waypoint(s) (%zu released), ending at '%s'",
                name.c_str(), order_id.c_str(), route.size(),    std::min(released_count.value_or(route.size()), route.size()),    route.back().node_id.c_str());
    return {CommandStatus::queued, order_id};
}

std::vector<vda5050::RouteWaypoint> Connector::to_waypoints(
    const RobotContext &ctx, const std::vector<RoutePoint> &route, const std::string &map_id) const
{
    std::vector<vda5050::RouteWaypoint> waypoints;
    waypoints.reserve(route.size());
    for (const auto &p : route)
    {
        // Apply the lower of the graph and operator speed limits.
        std::optional<double> limit = p.speed_limit;
        if (ctx.operator_speed_limit.has_value())
        {
            limit = limit.has_value() ? std::min(*limit, *ctx.operator_speed_limit): ctx.operator_speed_limit;
        }

        warn_if_unroutable(ctx, p.node_id, p.x, p.y, p.theta, map_id, limit);
        const auto robot_pose = ctx.transform.to_robot(p.x, p.y, p.theta);
        waypoints.push_back(vda5050::RouteWaypoint{p.node_id, {robot_pose[0], robot_pose[1], robot_pose[2]}, limit});
    }
    return waypoints;
}

bool Connector::order_allowed(RobotContext &ctx, const vda5050::RobotPose &base,  const std::vector<vda5050::RouteWaypoint> &route,  const std::string &map_id, std::size_t node_count,  std::size_t edge_count)
{
    vda5050::OrderShape shape;
    shape.node_count = node_count;
    shape.edge_count = edge_count;
    shape.map_id = map_id;
    shape.poses.push_back({base.x, base.y, base.theta});
    for (const auto &w : route)
    {
        shape.poses.push_back({w.pose.x, w.pose.y, w.pose.theta});
    }
    const auto now = std::chrono::steady_clock::now();
    if (ctx.last_order_time != std::chrono::steady_clock::time_point{})
    {
        shape.seconds_since_last_order = std::chrono::duration<double>(now - ctx.last_order_time).count();
    }

    std::vector<std::string> known_maps;
    if (ctx.last_state.has_value())
    {
        for (const auto &m : ctx.last_state->maps)
        {
            const std::string id = m.value("mapId", std::string{});
            if (!id.empty())
            {
                known_maps.push_back(id);
            }
        }
    }

    bool reject = false;
    for (const auto &v : vda5050::check_order(shape, ctx.factsheet, known_maps))
    {
        const bool hard = v.severity == vda5050::Severity::hard;
        if (hard)
        {
            RCLCPP_ERROR(_logger, "[VDA5050] %s: %s", ctx.name.c_str(), v.message.c_str());
        }
        else
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: %s", ctx.name.c_str(), v.message.c_str());
        }
        reject = reject || (hard && _strict_validation);
    }
    if (!reject)
    {
        ctx.last_order_time = now;
    }
    return !reject;
}

Connector::ReplanResult Connector::replan_route(const std::string &name,
                                                const std::vector<RoutePoint> &route,
                                                const std::string &map_id,
                                                std::optional<std::size_t> released_count)
{
    ReplanResult result;
    if (route.empty())
    {
        return result;
    }
    const auto declined = [&](const std::string &reason) {
        RCLCPP_INFO(_logger, "[VDA5050] %s: not stitching (%s)", name.c_str(), reason.c_str());
        return result;
    };

    const auto order_lock = lock_orders(name);

    std::string manufacturer, serial, interface_name, order_id, base_id, order_map;
    vda5050::RobotPose base{};
    vda5050::NodeDeviation deviation;
    std::vector<vda5050::RouteWaypoint> combined;
    int header_id = 0;
    int update_id = 0;
    std::size_t stitch_index = 0;
    std::size_t new_released = 0;
    std::size_t consumed = 0;
    std::size_t leading = 0;
    bool unchanged = false;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return result;
        }
        RobotContext &ctx = *it->second;

        // Only an acknowledged, unfinished order can be extended.
        if (ctx.current_order_id.empty() || ctx.current_route.empty() || !ctx.last_state.has_value())
        {
            return declined("no live order");
        }
        if (ctx.last_state->order_id != ctx.current_order_id || !ctx.last_state->last_node_sequence_id.has_value())
        {
            return declined("the AGV has not acknowledged the order");
        }
        if (!ctx.last_state->has_position())
        {
            return declined("the AGV has no valid pose");
        }
        if (!map_id.empty() && map_id != ctx.current_map_id)
        {
            return declined("map changed");
        }
        const std::size_t traversed = static_cast<std::size_t>(*ctx.last_state->last_node_sequence_id) / 2;
        if (traversed >= ctx.current_route.size())
        {
            return declined("the AGV is at the last node");
        }

        const auto new_route = to_waypoints(ctx, route, ctx.current_map_id);
        const auto plan = vda5050::plan_stitch(ctx.current_route, ctx.current_released_count, traversed, new_route);
        if (!plan.has_value())
        {
            const auto join = [](const std::vector<vda5050::RouteWaypoint> &points, std::size_t first,
                                 std::size_t last) {
                std::string out;
                for (std::size_t i = first; i < std::min(points.size(), last); ++i)
                {
                    out += (out.empty() ? "" : " ") + points[i].node_id;
                }
                return out;
            };
            return declined("new route does not repeat the released part: released " +
                            std::to_string(ctx.current_released_count) + ", passed " + std::to_string(traversed) +
                            ", order [" + join(ctx.current_route, traversed, ctx.current_released_count) +
                            "], new route [" + join(new_route, 0, 8) + "]");
        }

        consumed = plan->consumed;
        leading = plan->leading;
        stitch_index = plan->stitch_index;
        unchanged = plan->unchanged;
        combined = plan->route;
        new_released = vda5050::stitched_released_count(*plan, ctx.current_released_count, released_count.value_or(route.size()));
        order_id = ctx.current_order_id;
        order_map = ctx.current_map_id;
        base_id = ctx.current_base_id;
        base = ctx.current_base;
        manufacturer = ctx.manufacturer;
        serial = ctx.serial;
        interface_name = ctx.interface_name;
        deviation = _node_deviation;

        if (!unchanged)
        {
            // Only the nodes from the stitch node onward travel in the update.
            const std::size_t update_edges = combined.size() - stitch_index;
            if (!order_allowed(ctx, base, combined, order_map, update_edges + 1, update_edges))
            {
                return result;
            }
            header_id = ctx.next_order_header();
            update_id = ++ctx.order_update_id;
        }
    }

    if (!unchanged)
    {
        const auto order = vda5050::build_route_order(header_id, order_id, manufacturer, serial, base_id, base,
                                                      combined, order_map, update_id, new_released, stitch_index, deviation);
        if (publish_raw(vda5050::topic(interface_name, manufacturer, serial, vda5050::TOPIC_ORDER),
                        order.dump()) == CommandStatus::transport_failed)
        {
            RCLCPP_ERROR(_logger, "[VDA5050] %s -> order '%s' update %d NOT published (transport failure)", name.c_str(), order_id.c_str(), update_id);
            result.status = CommandStatus::transport_failed;
            return result;
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it != _robots.end())
        {
            RobotContext &ctx = *it->second;
            ctx.current_route = combined;
            ctx.order_done = false;
            ctx.current_released_count = new_released;
            ctx.target_node_id = combined.back().node_id;
            ctx.route_offset = consumed;
            result.released = new_released > consumed ? new_released - consumed : 0;
        }
    }

    const std::string sent = unchanged ? "route unchanged, nothing sent" : "update " + std::to_string(update_id);
    RCLCPP_INFO(_logger, "[VDA5050] %s replanned onto order '%s' (%s): stitched at sequence %zu, %zu point(s) after it",    name.c_str(), order_id.c_str(), sent.c_str(), 2 * stitch_index, combined.size() - stitch_index);
    result.stitched = true;
    result.order_id = order_id;
    result.consumed = consumed;
    result.leading_dropped = leading;
    return result;
}

CommandStatus Connector::release_more(const std::string &name, const std::string &expected_order_id,
                                      std::size_t released_count)
{
    const auto order_lock = lock_orders(name);

    std::string order_id, base_id, manufacturer, serial, interface_name, map_id;
    std::vector<vda5050::RouteWaypoint> waypoints;
    vda5050::RobotPose base{};
    vda5050::NodeDeviation deviation;
    int header_id = 0;
    int order_update_id = 0;
    std::size_t clamped = 0;
    std::size_t stitch_index = 0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] release_more: unknown robot '%s'", name.c_str());
            return CommandStatus::rejected;
        }
        RobotContext &ctx = *it->second;

        // RMF counts released points from the start of its current path.
        clamped = std::min(released_count + ctx.route_offset, ctx.current_route.size());
        if (ctx.current_order_id.empty() || ctx.current_order_id != expected_order_id || ctx.current_route.empty() ||
            clamped <= ctx.current_released_count)
        {
            return CommandStatus::rejected;
        }

        order_id = ctx.current_order_id;
        order_update_id = ++ctx.order_update_id;
        // The update starts at the last node released so far.
        stitch_index = ctx.current_released_count;
        waypoints = ctx.current_route;
        base_id = ctx.current_base_id;
        base = ctx.current_base;
        map_id = ctx.current_map_id;
        deviation = _node_deviation;
        header_id = ctx.next_order_header();
        manufacturer = ctx.manufacturer;
        serial = ctx.serial;
        interface_name = ctx.interface_name;
    }

    const auto order = vda5050::build_route_order(
        header_id, order_id, manufacturer, serial, base_id, base, waypoints, map_id,    order_update_id, clamped, stitch_index, deviation);

    const std::string order_topic =    vda5050::topic(interface_name, manufacturer, serial, vda5050::TOPIC_ORDER);
    const CommandStatus status = publish_raw(order_topic, order.dump());

    if (status == CommandStatus::transport_failed)
    {
        // Keep orderUpdateId increasing even when an update is not queued.
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> order '%s' update %d NOT published (transport failure)",     name.c_str(), order_id.c_str(), order_update_id);
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

    RCLCPP_INFO(_logger,    "[VDA5050] %s -> order '%s' update %d: released %zu/%zu route point(s), stitch at sequence %zu",
                name.c_str(), order_id.c_str(), order_update_id, clamped, waypoints.size(), 2 * stitch_index);
    return status;
}

bool Connector::order_in_progress(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _robots.find(name);
    return it != _robots.end() && !it->second->current_order_id.empty() && !it->second->order_done;
}

bool Connector::set_speed_limit(const std::string &name, std::optional<double> limit)
{
    if (limit.has_value() && (!std::isfinite(*limit) || *limit <= 0.0))
    {
        RCLCPP_ERROR(_logger, "[VDA5050] %s: speed limit %.3f is not a usable speed -- pass no "
                     "limit to clear the cap instead", name.c_str(), *limit);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] set_speed_limit: unknown robot '%s'",     name.c_str());
            return false;
        }
        it->second->operator_speed_limit = limit;
    }

    if (limit.has_value())
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s: operator speed limit set to %.2f m/s -- applies to the "    "next order, not the one already being driven", name.c_str(), *limit);
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
    const auto order_lock = lock_orders(name);

    std::string topic;
    nlohmann::json msg;
    std::string cancelled_order;
    std::string action_id;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return CommandStatus::transport_failed;
        }
        RobotContext &ctx = *it->second;

        cancelled_order = ctx.current_order_id;
        msg = vda5050::build_cancel_order(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, blocking_type_for(ctx, "cancelOrder", "HARD"));
        action_id = msg["actions"][0].value("actionId", std::string{});
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);
    }

    const CommandStatus status = publish_raw(topic, msg.dump());
    if (status == CommandStatus::transport_failed)
    {
        // Keep the active order when cancelOrder cannot be queued.
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
            if (_cancel_policy.confirm_timeout > std::chrono::seconds::zero() && !cancelled_order.empty())
            {
                ctx.cancel.sent(action_id, cancelled_order, std::chrono::steady_clock::now());
            }
            else
            {
                ctx.cancel.clear();
            }
        }
    }
    RCLCPP_INFO(_logger, "[VDA5050] %s -> cancelOrder", name.c_str());
    return status;
}

std::string Connector::init_position(const std::string &name, double x, double y,     double theta, const std::string &map_id)
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
                        "tracked -- AGVs commonly refuse to re-localize mid-order",    name.c_str(), ctx.current_order_id.c_str());
        }
    }
    if (publish_raw(topic, request.message.dump()) == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> initPosition NOT published (transport failure)", name.c_str());
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

        msg = vda5050::build_start_pause(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, blocking_type_for(ctx, "startPause", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,   vda5050::TOPIC_INSTANT_ACTIONS);
    }
    const CommandStatus status = publish_raw(topic, msg.dump());
    if (status == CommandStatus::queued)
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s -> startPause", name.c_str());
    }
    else
    {
        RCLCPP_ERROR(_logger, "[VDA5050] %s -> startPause NOT published (transport failure)", name.c_str());
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

        if (const auto violation = vda5050::check_instant_action(action_type, ctx.factsheet))
        {
            if (violation->severity == vda5050::Severity::hard && _strict_validation)
            {
                RCLCPP_ERROR(_logger, "[VDA5050] %s: %s -- not sent", name.c_str(), violation->message.c_str());
                return {};
            }
            RCLCPP_WARN(_logger, "[VDA5050] %s: %s -- sending it anyway", name.c_str(), violation->message.c_str());
        }
        else if (ctx.factsheet.has_value() && !ctx.factsheet->supports_scope(action_type, "INSTANT"))
        {
            // Publish only actions supported in the INSTANT scope.
            RCLCPP_WARN(_logger,"[VDA5050] %s: action '%s' is scoped NODE/EDGE only in the AGV's "
                        "factsheet, not INSTANT -- sending it as an instantAction anyway",    name.c_str(), action_type.c_str());
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

void Connector::set_strict_validation(bool strict)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _strict_validation = strict;
}

void Connector::set_stale_state_streak(int streak)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _stale_state_streak = streak;
}

void Connector::set_cancel_policy(const vda5050::CancelPolicy &policy)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _cancel_policy = policy;
}

void Connector::set_link_policy(const LinkPolicy &policy)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _link_policy = policy;
}

void Connector::set_node_deviation(const vda5050::NodeDeviation &deviation)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _node_deviation = deviation;
}

double Connector::state_timeout_for(const RobotContext &ctx) const
{
    double timeout = _link_policy.state_timeout_s;
    if (ctx.factsheet.has_value() && ctx.factsheet->default_state_interval.has_value())
    {
        timeout = std::max(timeout, *ctx.factsheet->default_state_interval * _link_policy.offline_state_intervals);
    }
    return timeout;
}

void Connector::resolve_pending_cancel(const std::string &name)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _robots.find(name);
        if (it == _robots.end() || !it->second->cancel.pending())
        {
            return;
        }
    }

    const auto order_lock = lock_orders(name);
    std::string topic;
    nlohmann::json message;
    std::string resent_action_id;
    std::string resend_note;
    std::vector<LogLine> log;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _robots.find(name);
        if (it == _robots.end() || !it->second->last_state.has_value())
        {
            return;
        }
        RobotContext &ctx = *it->second;
        const std::string order = ctx.cancel.order_id();
        const int attempts = ctx.cancel.attempts();
        const std::string prefix = "[VDA5050] " + ctx.name + ": cancelOrder for order '" + order + "'";
        const std::string waited = std::to_string(_cancel_policy.confirm_timeout.count()) + " s";

        using Outcome = vda5050::CancelTracker::Outcome;
        using Level = LogLine::Level;
        const auto verdict = ctx.cancel.assess(*ctx.last_state, ctx.last_state_time, std::chrono::steady_clock::now(), _cancel_policy);
        switch (verdict.outcome)
        {
            case Outcome::none:
                break;
            case Outcome::finished:
                log.push_back({Level::info, prefix + " finished"});
                break;
            case Outcome::failed:
                log.push_back({Level::warn, prefix + " was refused by the AGV: " + (verdict.detail.empty() ? "no reason given" : verdict.detail)});
                break;
            case Outcome::order_gone:
                log.push_back({Level::info, prefix + ": the AGV no longer reports the order"});
                break;
            case Outcome::still_running:
                log.push_back({Level::warn, prefix + " is still " + verdict.detail + " after " + waited});
                break;
            case Outcome::unanswered:
                log.push_back({Level::error, prefix + " was not answered after " + std::to_string(attempts) +
                                                 " attempt(s) and the AGV still reports the order"});
                break;
            case Outcome::resend:
                message = vda5050::build_cancel_order(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial,
                                                      blocking_type_for(ctx, "cancelOrder", "HARD"));
                resent_action_id = message["actions"][0].value("actionId", std::string{});
                topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);
                resend_note = prefix + " was not answered within " + waited + " while the AGV still reports the order; sending it again (attempt " +
                              std::to_string(attempts + 1) + "/" + std::to_string(_cancel_policy.attempts) + ")";
                break;
        }
    }

    if (!topic.empty() && publish_raw(topic, message.dump()) == CommandStatus::queued)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto it = _robots.find(name);
            if (it != _robots.end())
            {
                it->second->cancel.resent(resent_action_id, std::chrono::steady_clock::now());
            }
        }
        log.push_back({LogLine::Level::warn, resend_note});
    }
    write_log(log);
}

void Connector::poll(const std::string &name)
{
    resolve_pending_cancel(name);

    std::string topic;
    nlohmann::json message;
    int attempt = 0;
    int attempts_allowed = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return;
        }
        RobotContext &ctx = *it->second;

        // Ask for the factsheet when the retained one never arrived.
        attempts_allowed = _link_policy.factsheet_request_attempts;
        if (ctx.factsheet.has_value() || ctx.connected != true || ctx.factsheet_requests >= attempts_allowed)
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> wait(ctx.factsheet_requests == 0 ? _link_policy.factsheet_first_wait_s : _link_policy.factsheet_retry_wait_s);
        if (now - ctx.factsheet_wait_since < wait)
        {
            return;
        }
        ctx.factsheet_wait_since = now;
        attempt = ++ctx.factsheet_requests;

        message = vda5050::build_instant_action(ctx.next_instant_actions_header(), ctx.manufacturer,    ctx.serial, "factsheetRequest", nlohmann::json::object(), "NONE").message;
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, vda5050::TOPIC_INSTANT_ACTIONS);
    }

    if (publish_raw(topic, message.dump()) == CommandStatus::queued)
    {
        RCLCPP_INFO(_logger, "[VDA5050] %s: no factsheet received -- sent factsheetRequest (%d/%d)", name.c_str(), attempt, attempts_allowed);
    }
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

std::optional<Connector::TopicLevels> Connector::parse_topic(const std::string &topic) const
{
    const auto parts = split_topic(topic);
    if (parts.size() != 5 || parts[0] != _interface_name || parts[1] != vda5050::TOPIC_VERSION || parts[2].empty() ||
        parts[3].empty())
    {
        return std::nullopt;
    }
    return TopicLevels{parts[2], parts[3], parts[4]};
}

std::shared_ptr<Connector::RobotContext> Connector::find_by_identity(const TopicLevels &levels) const
{
    const auto it = _robot_index.find(levels.manufacturer + "/" + levels.serial);
    return it == _robot_index.end() ? nullptr : it->second;
}

Connector::OrderLock Connector::lock_orders(const std::string &name)
{
    OrderLock guard;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _robots.find(name);
        if (it == _robots.end())
        {
            return guard;
        }
        guard.robot = it->second;
    }
    guard.lock = std::unique_lock<std::mutex>(guard.robot->order_mutex);
    return guard;
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

    // Enforce the motion rules for NONE, SOFT, and HARD actions.
    if (s.driving && blocking_type != "NONE")
    {
        RCLCPP_WARN(_logger,"[VDA5050] %s: sending '%s' (blockingType %s) while the AGV is still "
                    "driving -- %s actions expect it stationary and may be queued or " "rejected",
                    ctx.name.c_str(), action_type.c_str(), blocking_type.c_str(), blocking_type.c_str());
    }

    if (!ctx.factsheet.has_value())
    {
        return;
    }

    // A running HARD action has exclusive control.
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
        // Report conflicts only when the blocking type is known.
        const auto it = ctx.factsheet->agv_actions.find(running_type);
        if (it != ctx.factsheet->agv_actions.end() &&    it->second.blocking_types.size() == 1 && it->second.blocking_types.front() == "HARD")
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: sending '%s' while '%s' (action %s) is still %s " "and only ever runs HARD -- it holds exclusivity, this action "  "may be queued or rejected",
                        ctx.name.c_str(), action_type.c_str(), running_type.c_str(),
                        running.value("actionId", std::string{}).c_str(), status.c_str());
        }
    }
}

void Connector::warn_if_map_mismatch(const RobotContext &ctx, const std::string &order_map_id) const
{
    if (order_map_id.empty() || !ctx.last_state.has_value() || ctx.last_state->map_id.empty())
    {
        return;
    }
    if (ctx.last_state->map_id != order_map_id)
    {
        RCLCPP_WARN(_logger,  "[VDA5050] %s: order mapId '%s' does not match the AGV's own "
                    "reported mapId '%s' -- RMF's nav graph and the robot's map "   "config may have drifted apart",  ctx.name.c_str(), order_map_id.c_str(), ctx.last_state->map_id.c_str());
    }
}

std::string Connector::blocking_type_for(const RobotContext &ctx,const std::string &action_type,const std::string &preferred)
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
        _metrics.publish_failed.add();
        RCLCPP_WARN(_logger, "[VDA5050] publish failed, dropping message to %s", topic.c_str());
        return CommandStatus::transport_failed;
    }
    _metrics.published.add();
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
        msg = vda5050::build_state_request(ctx.next_instant_actions_header(),  ctx.manufacturer, ctx.serial);
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,  vda5050::TOPIC_INSTANT_ACTIONS);
    }
    publish_raw(topic, msg.dump());
}

void Connector::on_connected()
{
    RCLCPP_INFO(_logger, "[VDA5050] MQTT (re)connected");

    // Ask every registered robot for a fresh state.
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto &[name, ctx] : _robots)
        {
            names.push_back(name);
        }
    }
    for (const auto &name : names)
    {
        request_state(name);
    }
}

void Connector::on_connection_lost(const std::string &cause)
{
    RCLCPP_WARN(_logger, "[VDA5050] MQTT connection lost: %s", cause.c_str());
}

void Connector::on_error(const std::string &context, const std::string &what)
{
    if (const auto held = admit_repeated("error " + context))
    {
        RCLCPP_WARN(_logger, "[VDA5050] %s: %s%s", context.c_str(), what.c_str(), suppressed_note(*held).c_str());
    }
}

std::optional<std::size_t> Connector::admit_repeated(const std::string &key)
{
    const auto held = _repeat_log.admit(key, std::chrono::steady_clock::now());
    if (!held)
    {
        _metrics.log_suppressed.add();
    }
    return held;
}

nlohmann::json Connector::metrics()
{
    std::size_t registered = 0;
    std::size_t online = 0;
    std::size_t without_state = 0;
    std::size_t with_state = 0;
    double age_max = 0.0;
    double age_sum = 0.0;
    double state_timeout_s = 0.0;
    std::string oldest;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto now = std::chrono::steady_clock::now();
        state_timeout_s = _link_policy.state_timeout_s;
        registered = _robots.size();
        for (const auto &[name, ctx] : _robots)
        {
            if (!ctx->last_state.has_value())
            {
                ++without_state;
                continue;
            }
            const double age = std::chrono::duration<double>(now - ctx->last_state_time).count();
            ++with_state;
            age_sum += age;
            if (age > age_max)
            {
                age_max = age;
                oldest = name;
            }
            if (ctx->connected != false && age <= state_timeout_for(*ctx))
            {
                ++online;
            }
        }
    }
    const auto rounded = [](double value) { return std::round(value * 1000.0) / 1000.0; };
    const mqtt::MqttClient::Stats link = _mqtt_client.stats();

    return {
        {"robots",
         {{"registered", registered},
          {"online", online},
          {"without_state", without_state},
          {"state_timeout_s", state_timeout_s},
          {"state_age_max_s", rounded(age_max)},
          {"state_age_mean_s", rounded(with_state == 0 ? 0.0 : age_sum / static_cast<double>(with_state))},
          {"oldest_state_robot", oldest}}},
        {"rx",
         {{"state", _metrics.rx_state.value()},
          {"visualization", _metrics.rx_visualization.value()},
          {"connection", _metrics.rx_connection.value()},
          {"factsheet", _metrics.rx_factsheet.value()},
          {"unregistered", _metrics.unregistered.value()}}},
        {"dropped",
         {{"bad_payload", _metrics.bad_payload.value()},
          {"bad_topic", _metrics.bad_topic.value()},
          {"invalid_state", _metrics.invalid_state.value()},
          {"stale_state", _metrics.stale_state.value()},
          {"oversize", link.oversize_dropped}}},
        {"log_suppressed", _metrics.log_suppressed.value()},
        {"published", {{"ok", _metrics.published.value()}, {"failed", _metrics.publish_failed.value()}}},
        {"mqtt",
         {{"connected", _mqtt_client.is_connected()},
          {"connects", link.connects},
          {"connections_lost", link.connections_lost},
          {"errors", link.errors}}},
        {"latency_us",
         {{"handle_state", util::to_json(_metrics.handle_state.take())},
          {"handle_other", util::to_json(_metrics.handle_other.take())},
          {"mutex_wait", util::to_json(_metrics.mutex_wait.take())}}},
        {"state_transit_us", util::to_json(_metrics.state_transit.take())},
    };
}

Connector::StateKeys Connector::make_state_keys(const vda5050::ParsedState &state)
{
    StateKeys keys;
    for (const auto &e : state.errors)
    {
        keys.errors += e.dump() + ";";
    }
    keys.safety = state.safety_state.e_stop + (state.safety_state.field_violation ? "|field" : "");
    for (const auto &i : state.information)
    {
        keys.information += i.dump() + ";";
    }
    for (const auto &l : state.loads)
    {
        keys.loads += l.value("loadId", std::string{"?"}) + "(" + l.value("loadType", std::string{"?"}) + ");";
    }
    for (const auto &m : state.maps)
    {
        keys.maps += m.value("mapId", std::string{"?"}) + ":" + m.value("mapStatus", std::string{"?"}) + ";";
    }
    return keys;
}

void Connector::write_log(const std::vector<LogLine> &log) const
{
    for (const auto &line : log)
    {
        switch (line.level)
        {
            case LogLine::Level::info:
                RCLCPP_INFO(_logger, "%s", line.text.c_str());
                break;
            case LogLine::Level::warn:
                RCLCPP_WARN(_logger, "%s", line.text.c_str());
                break;
            case LogLine::Level::error:
                RCLCPP_ERROR(_logger, "%s", line.text.c_str());
                break;
        }
    }
}

void Connector::update_state(RobotContext &ctx, const nlohmann::json &raw)
{
    if (!has_required_state_fields(raw))
    {
        _metrics.invalid_state.add();
        if (const auto held = admit_repeated("state " + ctx.name))
        {
            RCLCPP_WARN(_logger,"[VDA5050] %s: state message missing/mistyping a VDA5050-required "
                        "field (orderId/lastNodeId/driving/nodeStates/edgeStates/"
                        "actionStates/errors/operatingMode/safetyState.{eStop,fieldViolation}/"
                        "batteryState.batteryCharge) "
                        "-- rejecting rather than caching it with fallback values%s",
                        ctx.name.c_str(), suppressed_note(*held).c_str());
        }
        return;
    }

    vda5050::ParsedState state(raw);
    if (state.timestamp_ms.has_value())
    {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        _metrics.state_transit.record(std::chrono::milliseconds(now_ms - *state.timestamp_ms));
    }
    const StateKeys keys = make_state_keys(state);

    std::vector<LogLine> log;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (ctx.state_sequence.is_stale(state.header_id, state.timestamp_ms, _stale_state_streak))
        {
            _metrics.stale_state.add();
            report_stale_state(ctx, state, log);
        }
        else
        {
            ctx.last_state = std::move(state);
            ctx.last_state_time = std::chrono::steady_clock::now();
            if (!ctx.last_state->last_node_id.empty())
            {
                ctx.last_node_id = ctx.last_state->last_node_id;
            }
            const auto &sequence = ctx.last_state->last_node_sequence_id;
            if (!ctx.current_order_id.empty() && ctx.last_state->order_finished(ctx.current_order_id, ctx.target_node_id) &&
                (!sequence.has_value() || *sequence == 2 * ctx.current_route.size()))
            {
                ctx.order_done = true;
            }
            report_state_changes(ctx, keys, log);
        }
    }
    write_log(log);
}

void Connector::report_stale_state(const RobotContext &ctx, const vda5050::ParsedState &state, std::vector<LogLine> &log)
{
    const auto held = admit_repeated("stale " + ctx.name);
    if (!held)
    {
        return;
    }
    log.push_back({LogLine::Level::warn, "[VDA5050] " + ctx.name + ": dropped " + std::to_string(*held + 1) +
                                             " stale state message(s) -- headerId " +
                                             std::to_string(state.header_id.value_or(0)) + " after " +
                                             std::to_string(ctx.state_sequence.last_header_id().value_or(0))});
}

void Connector::report_state_changes(RobotContext &ctx, const StateKeys &keys, std::vector<LogLine> &log)
{
    const auto &s = *ctx.last_state;
    const std::string prefix = "[VDA5050] " + ctx.name;
    using Level = LogLine::Level;

    // Report changes in pause state.
    if (s.paused != ctx.last_paused)
    {
        log.push_back({Level::info, prefix + (s.paused ? " paused" : " resumed")});
        ctx.last_paused = s.paused;
    }

    // Report changes in errors.
    if (keys.errors != ctx.last_errors_key)
    {
        if (keys.errors.empty())
        {
            log.push_back({Level::info, prefix + ": errors cleared"});
        }
        else
        {
            log.push_back({Level::error, prefix + " reports: " + keys.errors});
        }
        ctx.last_errors_key = keys.errors;
    }

    // Report changes in safety state.
    if (keys.safety != ctx.last_safety_key)
    {
        if (s.safety_state.triggered())
        {
            log.push_back({Level::error, prefix + " SAFETY: eStop '" + s.safety_state.e_stop + "'" +
                                             (s.safety_state.field_violation ? ", protective field violated" : "") +
                                             " -- the AGV will not drive"});
        }
        else if (!ctx.last_safety_key.empty())
        {
            log.push_back({Level::info, prefix + " safety cleared"});
        }
        ctx.last_safety_key = keys.safety;
    }

    // Report changes in operating mode.
    if (s.operating_mode != ctx.last_mode_key)
    {
        if (s.operable())
        {
            log.push_back({Level::info, prefix + " operating mode: " + s.operating_mode});
        }
        else
        {
            log.push_back({Level::warn, prefix + " operating mode: " + s.operating_mode +
                                            " -- under local control, it will not act on orders from this fleet adapter"});
        }
        ctx.last_mode_key = s.operating_mode;
    }

    if (s.new_base_request != ctx.last_new_base_request)
    {
        if (s.new_base_request)
        {
            // Log horizon requests; the route schedule determines actual release.
            const std::size_t total = ctx.current_route.size();
            const std::string order = ctx.current_order_id.empty() ? "(none)" : ctx.current_order_id;
            const std::string counts = std::to_string(ctx.current_released_count) + "/" + std::to_string(total);
            if (ctx.current_released_count >= total)
            {
                log.push_back({Level::warn, prefix + " requests a new base but order '" + order +
                                                "' has no more route points to release (" + counts +
                                                " already released) -- the AGV's own base tracking may have diverged "
                                                "from this adapter's"});
            }
            else
            {
                log.push_back({Level::info, prefix + " requests a new base -- waiting at the release boundary of order '" +
                                                order + "' (" + counts + " route point(s) released)"});
            }
        }
        ctx.last_new_base_request = s.new_base_request;
    }

    // Report VDA5050 informational messages.
    if (keys.information != ctx.last_info_key)
    {
        if (!keys.information.empty())
        {
            log.push_back({Level::info, prefix + " info: " + keys.information});
        }
        ctx.last_info_key = keys.information;
    }

    // Report the current load set.
    if (keys.loads != ctx.last_loads_key)
    {
        log.push_back({Level::info, prefix + " loads: " + (keys.loads.empty() ? "(empty)" : keys.loads)});
        ctx.last_loads_key = keys.loads;
    }

    // Report maps declared by the AGV.
    if (keys.maps != ctx.last_maps_key)
    {
        if (!keys.maps.empty())
        {
            log.push_back({Level::info, prefix + " maps: " + keys.maps});
        }
        ctx.last_maps_key = keys.maps;
    }
}

void Connector::handle_message(const std::string &topic, const std::string &payload)
{
    util::ScopedTimer timer(&_metrics.handle_other);
    nlohmann::json raw;
    try
    {
        raw = nlohmann::json::parse(payload);
    }
    catch (const std::exception &)
    {
        _metrics.bad_payload.add();
        timer.retarget(nullptr);
        if (const auto held = admit_repeated("payload " + topic))
        {
            RCLCPP_WARN(_logger, "[VDA5050] bad payload on %s%s", topic.c_str(), suppressed_note(*held).c_str());
        }
        return;
    }

    const auto levels = parse_topic(topic);
    if (!levels)
    {
        _metrics.bad_topic.add();
        timer.retarget(nullptr);
        return;
    }

    const auto lock_requested = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(_mutex);
    _metrics.mutex_wait.record(std::chrono::steady_clock::now() - lock_requested);
    const std::shared_ptr<RobotContext> robot = find_by_identity(*levels);
    RobotContext *ctx = robot.get();
    if (!ctx)
    {
        _metrics.unregistered.add();
        record_unregistered(*levels, raw);
        return;
    }

    const std::string &leaf = levels->leaf;
    if (leaf == vda5050::TOPIC_STATE)
    {
        _metrics.rx_state.add();
        timer.retarget(&_metrics.handle_state);
        lock.unlock();
        update_state(*ctx, raw);
    }
    else if (leaf == vda5050::TOPIC_VISUALIZATION)
    {
        _metrics.rx_visualization.add();
        vda5050::ParsedVisualization viz(raw);
        // Ignore visualization messages without a usable pose.
        if (!viz.has_position())
        {
            return;
        }
        ctx->last_visualization = std::move(viz);
        ctx->last_visualization_time = std::chrono::steady_clock::now();
    }
    else if (leaf == vda5050::TOPIC_CONNECTION)
    {
        _metrics.rx_connection.add();
        const std::string conn = vda5050::read_string(raw, "connectionState").value_or("");
        const bool online = (conn == "ONLINE");
        if (online)
        {
            ctx->state_sequence.reset();
        }
        if (ctx->connected != online)
        {
            if (online)
            {
                RCLCPP_INFO(_logger, "[VDA5050] %s ONLINE", ctx->name.c_str());
                ctx->factsheet_requests = 0;
                ctx->factsheet_wait_since = std::chrono::steady_clock::now();
            }
            else if (ctx->connected == true)
            {
                RCLCPP_WARN(_logger, "[VDA5050] %s %s", ctx->name.c_str(), conn.empty() ? "OFFLINE" : conn.c_str());
            }
        }
        ctx->connected = online;
    }
    else if (leaf == vda5050::TOPIC_FACTSHEET)
    {
        _metrics.rx_factsheet.add();
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

        RCLCPP_INFO(_logger,"[VDA5050] %s factsheet: series '%s', %s/%s, speedMax %.2f, actions: %s", ctx->name.c_str(), fs.series_name.c_str(), fs.agv_kinematic.c_str(),
                    fs.agv_class.c_str(), fs.speed_max.value_or(0.0), actions.empty() ? "(none declared)" : actions.c_str());

        ctx->factsheet = std::move(fs);
        ctx->factsheet_requests = 0;
        const double offline_after = state_timeout_for(*ctx);
        if (offline_after > _link_policy.state_timeout_s)
        {
            RCLCPP_INFO(_logger, "[VDA5050] %s sends a state every %.1f s -- counts as offline after %.1f s without one",
                        ctx->name.c_str(), *ctx->factsheet->default_state_interval, offline_after);
        }
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

    // Use the fresher visualization pose and velocity when available.
    if (ctx.last_visualization.has_value() && ctx.last_visualization_time > ctx.last_state_time)
    {
        const auto &v = *ctx.last_visualization;
        // Keep pose and velocity from the same map frame.
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

    // Report order actions still running after navigation ends.
    if (s.node_states.empty() && s.edge_states.empty() && !s.driving && !s.actions_settled(ctx.order_action_ids))
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
            RCLCPP_INFO(_logger,"[VDA5050] %s reached the end of order '%s'; waiting on action(s): %s", name.c_str(), ctx.current_order_id.c_str(), pending.c_str());
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
                                "until the robot echoes the nodeId this adapter sends.", name.c_str(), ctx.current_order_id.c_str(),
                                s.last_node_id.empty() ? "(empty)" : s.last_node_id.c_str(), ctx.target_node_id.c_str());
        }
    }

    return false;
}

bool Connector::is_order_stuck(const std::string &name) const
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
    return since_dispatch > _link_policy.order_stuck_timeout_s;
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

bool Connector::is_online(const std::string &name)
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
    return age <= state_timeout_for(ctx);
}

namespace {

// Most robots one interface may show before further ones are ignored.
constexpr std::size_t kMaxDiscovered = 256;

}  // namespace

void Connector::record_unregistered(const TopicLevels &levels, const nlohmann::json &raw)
{
    if (!raw.is_object())
    {
        return;
    }
    const std::string &leaf = levels.leaf;
    if (leaf != vda5050::TOPIC_CONNECTION && leaf != vda5050::TOPIC_FACTSHEET && leaf != vda5050::TOPIC_STATE)
    {
        return;
    }

    const std::string key = levels.manufacturer + "/" + levels.serial;
    auto it = _discovered.find(key);
    if (it == _discovered.end())
    {
        if (_discovered.size() >= kMaxDiscovered)
        {
            // Retained messages of long-gone robots must not crowd out a robot that is online now.
            const auto stale = std::find_if(_discovered.begin(), _discovered.end(),
                                            [](const auto &entry) { return !entry.second.online; });
            if (stale == _discovered.end())
            {
                return;
            }
            _discovered.erase(stale);
        }
        it = _discovered.emplace(key, DiscoveredRobot{}).first;
        it->second.manufacturer = levels.manufacturer;
        it->second.serial = levels.serial;
    }
    DiscoveredRobot &robot = it->second;

    if (leaf == vda5050::TOPIC_CONNECTION)
    {
        robot.connection_seen = true;
        robot.online = vda5050::read_string(raw, "connectionState").value_or("") == "ONLINE";
    }
    else if (leaf == vda5050::TOPIC_FACTSHEET)
    {
        vda5050::ParsedFactsheet factsheet(raw);
        if (factsheet.has_content())
        {
            robot.factsheet = std::move(factsheet);
        }
    }
    else if (raw.contains("agvPosition") && raw["agvPosition"].is_object())
    {
        const auto &pos = raw["agvPosition"];
        const auto x = vda5050::read_number(pos, "x");
        const auto y = vda5050::read_number(pos, "y");
        robot.has_state = x.has_value() && y.has_value();
        robot.pose_initialized = vda5050::read_bool(pos, "positionInitialized").value_or(false);
        robot.x = x.value_or(0.0);
        robot.y = y.value_or(0.0);
        robot.theta = vda5050::read_number(pos, "theta").value_or(0.0);
        robot.map_id = vda5050::read_string(pos, "mapId").value_or("");
    }
}

std::vector<Connector::DiscoveredRobot> Connector::discovered() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<DiscoveredRobot> out;
    for (const auto &[key, robot] : _discovered)
    {
        out.push_back(robot);
    }
    return out;
}

std::optional<Connector::DiscoveredRobot> Connector::find_discovered(const std::string &manufacturer,
                                                                       const std::string &serial) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _discovered.find(manufacturer + "/" + serial);
    if (it == _discovered.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void Connector::watch_discovered()
{
    std::vector<std::string> topics;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto &[key, robot] : _discovered)
        {
            if (robot.watched || !robot.connection_seen || !robot.online)
            {
                continue;
            }
            robot.watched = true;
            for (const char *leaf : {vda5050::TOPIC_FACTSHEET, vda5050::TOPIC_STATE})
            {
                topics.push_back(vda5050::topic(_interface_name, robot.manufacturer, robot.serial, leaf));
            }
        }
    }
    for (const auto &topic : topics)
    {
        _mqtt_client.subscribe(topic, 1);
    }
}

std::vector<vda5050::ParsedFactsheet> Connector::registered_factsheets() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<vda5050::ParsedFactsheet> out;
    for (const auto &[name, ctx] : _robots)
    {
        if (ctx->factsheet.has_value())
        {
            out.push_back(*ctx->factsheet);
        }
    }
    return out;
}

}  // namespace vda5050_fleet_adapter_full_control::rmf
