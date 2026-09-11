#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"

#include <algorithm>
#include <stdexcept>

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

nlohmann::json build_route_order(
    int header_id, const std::string &order_id,
    const std::string &manufacturer, const std::string &serial,
    const std::string &base_node_id, const RobotPose &base,
    const std::vector<RouteWaypoint> &route,
    const std::string &map_id,
    int order_update_id,
    std::optional<std::size_t> released_count)
{
    if (order_id.empty())
    {
        throw std::invalid_argument("build_route_order: order_id must not be empty -- the caller "
            "must generate it before calling, so it can track the same id " "make_order() ends up sending.");
    }

    // Clamped, not trusted: a caller-supplied count past route.size()
    // would otherwise silently release nothing.
    const std::size_t release_edges = std::min(released_count.value_or(route.size()), route.size());

    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json edges = nlohmann::json::array();

    nodes.push_back(make_node(base_node_id, 0, base.x, base.y, base.theta, map_id));

    std::string previous_node_id = base_node_id;
    for (std::size_t i = 0; i < route.size(); ++i)
    {
        const auto &wp = route[i];
        // Nodes take even sequenceIds, the edges between them the odd ones
        // in between: base is 0, so waypoint i is node 2*(i+1) and reaches it over edge 2*i+1.
        const int edge_sequence = static_cast<int>(2 * i + 1);
        const int node_sequence = static_cast<int>(2 * (i + 1));
        const bool released = i < release_edges;

        edges.push_back(make_edge("e_" + previous_node_id + "_" + wp.node_id, edge_sequence,
                                  previous_node_id, wp.node_id, released, wp.speed_limit));

        nodes.push_back(
            make_node(wp.node_id, node_sequence, wp.pose.x, wp.pose.y, wp.pose.theta, map_id, released));

        previous_node_id = wp.node_id;
    }

    return make_order(header_id, manufacturer, serial, nodes, edges, order_id, order_update_id);
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
