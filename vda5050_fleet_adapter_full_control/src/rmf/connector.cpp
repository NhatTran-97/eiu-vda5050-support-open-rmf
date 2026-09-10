#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

#include <algorithm>
#include <cmath>
#include <unistd.h>

#include <rclcpp/logging.hpp>

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

namespace {

// Required for order_finished()/is_command_completed() to mean anything: a state message missing or mistyping these is not safe to cache, since
// get_array()'s "missing == empty" fallback would otherwise look
// indistinguishable from a genuinely drained order.
bool has_required_completion_fields(const nlohmann::json &raw)
{
    return raw.is_object() &&
           raw.contains("orderId") && raw["orderId"].is_string() &&
           raw.contains("lastNodeId") && raw["lastNodeId"].is_string() &&
           raw.contains("driving") && raw["driving"].is_boolean() &&
           raw.contains("nodeStates") && raw["nodeStates"].is_array() &&
           raw.contains("edgeStates") && raw["edgeStates"].is_array() &&
           raw.contains("actionStates") && raw["actionStates"].is_array();
}

}  // namespace

Connector::Connector(rclcpp::Logger logger, std::string broker_url,
                     std::string interface_name,
                     std::optional<std::string> username,
                     std::optional<std::string> password)
  : _logger(std::move(logger)),
    _interface_name(std::move(interface_name)),
    _mqtt_client(std::move(broker_url),
                "rmf_vda5050_adapter_" + std::to_string(::getpid()),
                std::move(username), std::move(password))
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

void Connector::navigate(const std::string &name, const std::string &dest_node_id,
                         double x, double y, double theta, const std::string &map_id,
                         std::optional<double> speed_limit)
{
    std::string order_id;
    std::string base_id;
    std::string manufacturer, serial, interface_name;
    vda5050::RobotPose dest{}, base{};
    int header_id = 0;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _robots.find(name);
        if (it == _robots.end())
        {
            RCLCPP_ERROR(_logger, "[VDA5050] navigate: unknown robot '%s'", name.c_str());
            return;
        }
        RobotContext &ctx = *it->second;

        // Warn, never refuse: EasyFullControl's CommandExecution has no
        // error() to report a rejected navigation with, so refusing to
        // publish would leave RMF waiting on a command that can never
        // finish. A malformed order at least surfaces as an AGV-side error
        // in the next state message.
        warn_if_unroutable(ctx, dest_node_id, x, y, theta, map_id, speed_limit);

        const auto robot_dest = ctx.transform.to_robot(x, y, theta);
        dest = {robot_dest[0], robot_dest[1], robot_dest[2]};
        base = dest;
        base_id = ctx.last_node_id.empty() ? (ctx.serial + "_start") : ctx.last_node_id;
        if (ctx.last_state.has_value() && ctx.last_state->has_position())
        {
            base = {*ctx.last_state->x, *ctx.last_state->y, *ctx.last_state->theta};
        }

        order_id = vda5050::make_uuid();
        ctx.current_order_id = order_id;
        ctx.target_node_id = dest_node_id;
        header_id = ctx.next_order_header();
        manufacturer = ctx.manufacturer;
        serial = ctx.serial;
        interface_name = ctx.interface_name;
    }

    const auto order = vda5050::build_navigate_order(
        header_id, order_id, manufacturer, serial,
        base_id, base, dest_node_id, dest, map_id, speed_limit);

    const std::string order_topic = vda5050::topic(interface_name, manufacturer, serial, vda5050::TOPIC_ORDER);
    publish_raw(order_topic, order.dump());
    RCLCPP_INFO(_logger, "[VDA5050] %s -> order '%s' to node '%s' (%.2f, %.2f)",
                name.c_str(), order_id.c_str(), dest_node_id.c_str(), dest.x, dest.y);
}

void Connector::stop(const std::string &name)
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

        msg = vda5050::build_cancel_order(ctx.next_instant_actions_header(),
                                          ctx.manufacturer, ctx.serial,
                                          blocking_type_for(ctx, "cancelOrder", "HARD"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);
        // Cleared here, synchronously, before publish -- so is_command_completed()
        // never asks order_finished() about an order we just cancelled.
        ctx.current_order_id.clear();
        ctx.target_node_id.clear();
    }
    publish_raw(topic, msg.dump());
    RCLCPP_INFO(_logger, "[VDA5050] %s -> cancelOrder", name.c_str());
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

        // Values go out as strings: make_action() carries every parameter as
        // a JSON string (see its own documented limitation). Clients that
        // parse them numerically -- as tb3_vda5050_bridge does with
        // std::stod -- accept this; a strictly typed third-party client may
        // not.
        const std::vector<std::pair<std::string, std::string>> params{
            {"x", std::to_string(robot_pose[0])},
            {"y", std::to_string(robot_pose[1])},
            {"theta", std::to_string(robot_pose[2])},
            {"mapId", map_id},
        };

        request = vda5050::build_instant_action(ctx.next_instant_actions_header(),
                                                ctx.manufacturer, ctx.serial,
                                                "initPosition", params,
                                                blocking_type_for(ctx, "initPosition", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);

        if (!ctx.current_order_id.empty())
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: sending initPosition while order '%s' is still "
                        "tracked -- AGVs commonly refuse to re-localize mid-order",
                        name.c_str(), ctx.current_order_id.c_str());
        }
    }
    publish_raw(topic, request.message.dump());
    RCLCPP_INFO(_logger, "[VDA5050] %s -> initPosition (%.2f, %.2f, %.2f) on '%s'",
                name.c_str(), x, y, theta, map_id.c_str());
    return request.action_id;
}

void Connector::pause(const std::string &name)
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

        msg = vda5050::build_start_pause(ctx.next_instant_actions_header(),
                                         ctx.manufacturer, ctx.serial,
                                         blocking_type_for(ctx, "startPause", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);
    }
    publish_raw(topic, msg.dump());
    RCLCPP_INFO(_logger, "[VDA5050] %s -> startPause", name.c_str());
}

void Connector::resume(const std::string &name)
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

        msg = vda5050::build_stop_pause(ctx.next_instant_actions_header(),
                                        ctx.manufacturer, ctx.serial,
                                        blocking_type_for(ctx, "stopPause", "NONE"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);
    }
    publish_raw(topic, msg.dump());
    RCLCPP_INFO(_logger, "[VDA5050] %s -> stopPause", name.c_str());
}

std::string Connector::execute_instant_action(
    const std::string &name, const std::string &action_type,
    const std::vector<std::pair<std::string, std::string>> &parameters)
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
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: action '%s' is not in the AGV's factsheet "
                        "(protocolFeatures.agvActions) -- sending it anyway",
                        name.c_str(), action_type.c_str());
        }

        request = vda5050::build_instant_action(ctx.next_instant_actions_header(),
                                                ctx.manufacturer, ctx.serial,
                                                action_type, parameters,
                                                blocking_type_for(ctx, action_type, "HARD"));
        topic = vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                               vda5050::TOPIC_INSTANT_ACTIONS);
    }
    publish_raw(topic, request.message.dump());
    return request.action_id;
}

void Connector::subscribe_robot(const RobotContext &ctx)
{
    for (const char *leaf :
         {vda5050::TOPIC_STATE, vda5050::TOPIC_CONNECTION, vda5050::TOPIC_VISUALIZATION,
          vda5050::TOPIC_FACTSHEET})
    {
        _mqtt_client.subscribe(
            vda5050::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, leaf), 1);
    }
}

Connector::RobotContext *Connector::match_robot(const std::string &topic)
{
    for (auto &[_, ctx] : _robots)
    {
        const std::string needle = "/" + ctx->manufacturer + "/" + ctx->serial + "/";
        if (topic.find(needle) != std::string::npos)
        {
            return ctx.get();
        }
    }
    return nullptr;
}

void Connector::warn_if_unroutable(const RobotContext &ctx,
                                   const std::string &dest_node_id,
                                   double x, double y, double theta,
                                   const std::string &map_id,
                                   std::optional<double> speed_limit) const
{
    if (dest_node_id.empty())
    {
        RCLCPP_WARN(_logger,
                    "[VDA5050] %s: navigating to an empty nodeId -- the AGV cannot "
                    "echo it back as lastNodeId, so this order can never complete",
                    ctx.name.c_str());
    }
    if (map_id.empty())
    {
        RCLCPP_WARN(_logger, "[VDA5050] %s: navigating with an empty mapId",
                    ctx.name.c_str());
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(theta))
    {
        RCLCPP_WARN(_logger,
                    "[VDA5050] %s: destination is not finite (%.3f, %.3f, %.3f)",
                    ctx.name.c_str(), x, y, theta);
    }

    if (speed_limit.has_value())
    {
        if (!std::isfinite(*speed_limit) || *speed_limit <= 0.0)
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: speed limit %.3f is not a usable speed",
                        ctx.name.c_str(), *speed_limit);
        }
        else if (ctx.factsheet.has_value() && ctx.factsheet->speed_max.has_value() &&
                 *speed_limit > *ctx.factsheet->speed_max)
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: speed limit %.3f exceeds the AGV's declared "
                        "speedMax %.3f (factsheet)",
                        ctx.name.c_str(), *speed_limit, *ctx.factsheet->speed_max);
        }
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

void Connector::publish_raw(const std::string &topic, const std::string &payload)
{
    if (!_mqtt_client.publish(topic, payload))
    {
        // publish() also returns false when Paho itself threw (see on_error()
        // for that detail) -- this line only means "not delivered", not
        // necessarily "offline".
        RCLCPP_WARN(_logger, "[VDA5050] publish failed, dropping message to %s", topic.c_str());
    }
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
        return topic.size() >= s.size() &&
               topic.compare(topic.size() - s.size(), s.size(), s) == 0;
    };

    if (ends_with(vda5050::TOPIC_STATE))
    {
        if (!has_required_completion_fields(raw))
        {
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s: state message missing required fields "
                        "(orderId/lastNodeId/driving/nodeStates/edgeStates/actionStates), ignoring",
                        ctx->name.c_str());
            return;
        }
        ctx->last_state = vda5050::ParsedState(raw);
        ctx->last_state_time = std::chrono::steady_clock::now();
        if (!ctx->last_state->last_node_id.empty())
        {
            ctx->last_node_id = ctx->last_state->last_node_id;
        }

        // Errors and pause were parsed and then dropped, so a faulted robot
        // still looked healthy to RMF and nothing appeared in the log.
        // Report on change.
        std::string errors_key;
        for (const auto &e : ctx->last_state->errors)
        {
            errors_key += e.dump() + ";";
        }
        if (ctx->last_state->paused)
        {
            errors_key += "paused;";
        }
        if (errors_key != ctx->last_errors_key)
        {
            if (errors_key.empty())
            {
                RCLCPP_INFO(_logger, "[VDA5050] %s: errors cleared", ctx->name.c_str());
            }
            else
            {
                RCLCPP_ERROR(_logger, "[VDA5050] %s reports: %s", ctx->name.c_str(),
                            errors_key.c_str());
            }
            ctx->last_errors_key = errors_key;
        }
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
                RCLCPP_WARN(_logger, "[VDA5050] %s %s", ctx->name.c_str(),
                            conn.empty() ? "OFFLINE" : conn.c_str());
            }
        }
        ctx->connected = online;
    }
    else if (ends_with(vda5050::TOPIC_FACTSHEET))
    {
        vda5050::ParsedFactsheet fs(raw);
        if (!fs.has_content())
        {
            RCLCPP_WARN(_logger, "[VDA5050] %s: factsheet carried nothing usable, ignoring",
                        ctx->name.c_str());
            return;
        }

        std::string actions;
        for (const auto &[type, blocking] : fs.agv_actions)
        {
            actions += actions.empty() ? "" : ", ";
            actions += type;
            if (!blocking.empty())
            {
                actions += "(";
                for (std::size_t i = 0; i < blocking.size(); ++i)
                {
                    actions += (i == 0 ? "" : "|") + blocking[i];
                }
                actions += ")";
            }
        }

        RCLCPP_INFO(_logger,
                    "[VDA5050] %s factsheet: series '%s', %s/%s, speedMax %.2f, actions: %s",
                    ctx->name.c_str(), fs.series_name.c_str(), fs.agv_kinematic.c_str(),
                    fs.agv_class.c_str(), fs.speed_max.value_or(0.0),
                    actions.empty() ? "(none declared)" : actions.c_str());

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
    const auto rmf_pos = ctx.transform.to_rmf(*s.x, *s.y, *s.theta);
    RobotData data;
    data.map_name = s.map_id;
    data.position = rmf_pos;
    data.battery_soc = std::clamp(s.battery_soc.value_or(1.0), 0.0, 1.0);
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

    if (s.order_finished(ctx.current_order_id, ctx.target_node_id))
    {
        return true;
    }

    // The order has drained and the robot has stopped, so the only thing
    // still failing is the final node check. That condition will never flip
    // on its own, so name it once instead of letting the navigation hang
    // without a word.
    if (s.node_states.empty() && s.edge_states.empty() &&
        !ctx.target_node_id.empty() && s.last_node_id != ctx.target_node_id)
    {
        const std::string key = ctx.current_order_id + "|" + s.last_node_id;
        if (ctx.last_incomplete_key != key)
        {
            ctx.last_incomplete_key = key;
            RCLCPP_WARN(_logger,
                        "[VDA5050] %s drained order '%s' but reports lastNodeId '%s' "
                        "while the order targeted '%s'. Navigation cannot complete "
                        "until the robot echoes the nodeId this adapter sends.",
                        name.c_str(), ctx.current_order_id.c_str(),
                        s.last_node_id.empty() ? "(empty)" : s.last_node_id.c_str(),
                        ctx.target_node_id.c_str());
        }
    }

    return false;
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
    const auto age = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - ctx.last_state_time).count();
    return age <= state_timeout_s;
}

}  // namespace vda5050_fleet_adapter_full_control::rmf
