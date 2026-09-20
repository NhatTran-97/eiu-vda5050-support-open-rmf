#include "vda5050_fleet_adapter_full_control/vda5050/order_validation.hpp"

#include <algorithm>
#include <cmath>

namespace vda5050_fleet_adapter_full_control::vda5050 {

std::vector<Violation> check_order(const OrderShape &order,
                                   const std::optional<ParsedFactsheet> &factsheet,
                                   const std::vector<std::string> &known_maps)
{
    std::vector<Violation> out;

    for (std::size_t i = 0; i < order.poses.size(); ++i)
    {
        const auto &p = order.poses[i];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
        {
            out.push_back({Severity::hard, "pose " + std::to_string(i) + " is not finite (" + std::to_string(p[0]) + ", " + std::to_string(p[1]) + ", " + std::to_string(p[2]) + ")"});
        }
    }

    if (!order.map_id.empty() && !known_maps.empty() &&
        std::find(known_maps.begin(), known_maps.end(), order.map_id) == known_maps.end())
    {
        out.push_back({Severity::hard, "mapId '" + order.map_id + "' is not among the maps the AGV reports"});
    }

    if (factsheet.has_value())
    {
        const auto &fs = *factsheet;
        if (fs.max_order_nodes.has_value() && order.node_count > *fs.max_order_nodes)
        {
            out.push_back({Severity::hard, "order has " + std::to_string(order.node_count) + " node(s), over the AGV's declared maxArrayLens['order.nodes'] (" +
                               std::to_string(*fs.max_order_nodes) + ")"});
        }
        if (fs.max_order_edges.has_value() && order.edge_count > *fs.max_order_edges)
        {
            out.push_back({Severity::hard, "order has " + std::to_string(order.edge_count) + " edge(s), over the AGV's declared maxArrayLens['order.edges'] (" +
                            std::to_string(*fs.max_order_edges) + ")"});
        }
        if (fs.min_order_interval.has_value() && order.seconds_since_last_order.has_value() && *order.seconds_since_last_order < *fs.min_order_interval)
        {
            out.push_back({Severity::soft, "order sent " + std::to_string(*order.seconds_since_last_order) + 
                "s after the previous one, under the AGV's minOrderInterval (" + std::to_string(*fs.min_order_interval) + "s)"});
        }
    }

    return out;
}

bool is_core_action(const std::string &action_type)
{
    static const char *const kCore[] = {"cancelOrder", "startPause",   "stopPause", "stateRequest", "initPosition", "factsheetRequest"};
    return std::any_of(std::begin(kCore), std::end(kCore), [&](const char *core) { return action_type == core; });
}

std::optional<Violation> check_instant_action(const std::string &action_type, const std::optional<ParsedFactsheet> &factsheet)
{
    if (!factsheet.has_value() || factsheet->supports_action(action_type))
    {
        return std::nullopt;
    }
    const std::string reason = "action '" + action_type + "' is not in the AGV's factsheet (protocolFeatures.agvActions)";
    return Violation{is_core_action(action_type) ? Severity::soft : Severity::hard, reason};
}

bool has_hard_violation(const std::vector<Violation> &violations)
{
    return std::any_of(violations.begin(), violations.end(), [](const Violation &v) { return v.severity == Severity::hard; });
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
