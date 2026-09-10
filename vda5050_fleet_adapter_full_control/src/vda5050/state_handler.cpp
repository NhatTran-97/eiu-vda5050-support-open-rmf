#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

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

}  // namespace

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
    }

    if (raw.contains("batteryState") && raw["batteryState"].is_object())
    {
        if (const auto charge = get_opt<double>(raw["batteryState"], "batteryCharge"))
        {
            battery_soc = *charge / 100.0;
        }
    }

    order_id = raw.value("orderId", std::string{});
    order_update_id = get_opt<std::uint32_t>(raw, "orderUpdateId"); 
    last_node_id = raw.value("lastNodeId", std::string{});
    driving = raw.value("driving", false);
    paused = raw.value("paused", false);

    node_states = get_array(raw, "nodeStates");
    edge_states = get_array(raw, "edgeStates");
    action_states = get_array(raw, "actionStates");
    errors = get_array(raw, "errors");
}

bool ParsedState::has_position() const
{
    return x.has_value() && y.has_value() && theta.has_value() && position_initialized;
}

bool ParsedState::order_finished(const std::string &oid,
                                 const std::string &target_node_id) const
{
    if (oid.empty() || order_id.empty() || order_id != oid)
    {
        return false;
    }
    if (!target_node_id.empty() && last_node_id != target_node_id)
    {
        return false;
    }
    return node_states.empty() && edge_states.empty() && !driving;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
