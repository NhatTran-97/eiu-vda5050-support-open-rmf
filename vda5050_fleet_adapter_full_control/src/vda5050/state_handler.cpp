#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

#include <cmath>

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

template <typename T>
std::optional<T> get_opt(const nlohmann::json &j, const char *key)
{
    if (j.contains(key) && !j.at(key).is_null())
    {
        return j.at(key).get<T>();
    }
    return std::nullopt;
}

std::vector<nlohmann::json> get_array(const nlohmann::json &j, const char *key)
{
    std::vector<nlohmann::json> out;
    if (j.contains(key) && j.at(key).is_array())
    {
        for (const auto &e : j.at(key))
        {
            out.push_back(e);
        }
    }
    return out;
}

// state.velocity and visualization.velocity share one schema.
std::optional<Velocity> parse_velocity(const nlohmann::json &raw)
{
    if (!raw.contains("velocity") || !raw["velocity"].is_object())
    {
        return std::nullopt;
    }
    const auto &v = raw["velocity"];
    Velocity parsed;
    parsed.vx = v.value("vx", 0.0);
    parsed.vy = v.value("vy", 0.0);
    parsed.omega = v.value("omega", 0.0);
    return parsed;
}

}  // namespace

double Velocity::speed() const
{
    return std::hypot(vx, vy);
}

bool is_terminal_action_status(const std::string &status)
{
    return status == "FINISHED" || status == "FAILED";
}

bool SafetyState::triggered() const
{
    return field_violation || (!e_stop.empty() && e_stop != "NONE");
}

ParsedState::ParsedState(const nlohmann::json &raw)
{
    if (raw.contains("agvPosition") && raw["agvPosition"].is_object())
    {
        const auto &pos = raw["agvPosition"];
        x = get_opt<double>(pos, "x");
        y = get_opt<double>(pos, "y");
        theta = get_opt<double>(pos, "theta");
        map_id = pos.value("mapId", std::string{});
        position_initialized = pos.value("positionInitialized", false);
        localization_score = get_opt<double>(pos, "localizationScore");
    }

    if (raw.contains("batteryState") && raw["batteryState"].is_object())
    {
        const auto &battery = raw["batteryState"];
        if (const auto charge = get_opt<double>(battery, "batteryCharge"))
        {
            battery_soc = *charge / 100.0;
        }
        charging = battery.value("charging", false);
    }

    velocity = parse_velocity(raw);

    if (raw.contains("safetyState") && raw["safetyState"].is_object())
    {
        const auto &s = raw["safetyState"];
        safety_state.e_stop = s.value("eStop", std::string{"NONE"});
        safety_state.field_violation = s.value("fieldViolation", false);
    }

    order_id = raw.value("orderId", std::string{});
    order_update_id = get_opt<std::uint32_t>(raw, "orderUpdateId");
    zone_set_id = raw.value("zoneSetId", std::string{});
    last_node_id = raw.value("lastNodeId", std::string{});
    last_node_sequence_id = get_opt<std::uint32_t>(raw, "lastNodeSequenceId");
    driving = raw.value("driving", false);
    paused = raw.value("paused", false);
    new_base_request = raw.value("newBaseRequest", false);
    distance_since_last_node = get_opt<double>(raw, "distanceSinceLastNode");
    operating_mode = raw.value("operatingMode", std::string{"AUTOMATIC"});

    node_states = get_array(raw, "nodeStates");
    edge_states = get_array(raw, "edgeStates");
    action_states = get_array(raw, "actionStates");
    errors = get_array(raw, "errors");
    information = get_array(raw, "information");
    loads = get_array(raw, "loads");
    maps = get_array(raw, "maps");
}

bool ParsedState::has_position() const
{
    return x.has_value() && y.has_value() && theta.has_value() && position_initialized;
}

bool ParsedState::operable() const
{
    return operating_mode == "AUTOMATIC" || operating_mode == "SEMIAUTOMATIC";
}

std::string ParsedState::first_fatal_error() const
{
    for (const auto &e : errors)
    {
        if (!e.is_object() || e.value("errorLevel", std::string{}) != "FATAL")
        {
            continue;
        }
        const std::string type = e.value("errorType", std::string{});
        return type.empty() ? "(unnamed FATAL error)" : type;
    }
    return {};
}

std::optional<std::string> ParsedState::action_status(const std::string &action_id) const
{
    for (const auto &a : action_states)
    {
        if (a.value("actionId", std::string{}) == action_id)
        {
            return a.value("actionStatus", std::string{});
        }
    }
    return std::nullopt;
}

bool ParsedState::actions_settled(const std::vector<std::string> &action_ids) const
{
    for (const auto &id : action_ids)
    {
        const auto status = action_status(id);
        if (status.has_value() && !is_terminal_action_status(*status))
        {
            return false;
        }
    }
    return true;
}

bool ParsedState::order_finished(const std::string &oid,
                                 const std::string &target_node_id,
                                 const std::vector<std::string> &order_action_ids) const
{
    if (oid.empty() || order_id.empty() || order_id != oid)
    {
        return false;
    }
    if (!target_node_id.empty() && last_node_id != target_node_id)
    {
        return false;
    }
    return node_states.empty() && edge_states.empty() && !driving && actions_settled(order_action_ids);
}

ParsedVisualization::ParsedVisualization(const nlohmann::json &raw)
{
    if (raw.contains("agvPosition") && raw["agvPosition"].is_object())
    {
        const auto &pos = raw["agvPosition"];
        x = get_opt<double>(pos, "x");
        y = get_opt<double>(pos, "y");
        theta = get_opt<double>(pos, "theta");
        map_id = pos.value("mapId", std::string{});
        position_initialized = pos.value("positionInitialized", false);
    }
    velocity = parse_velocity(raw);
}

bool ParsedVisualization::has_position() const
{
    return x.has_value() && y.has_value() && theta.has_value() && position_initialized;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
