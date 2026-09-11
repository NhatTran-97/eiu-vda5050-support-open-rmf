#ifndef ORDER_HANDLER_HPP
#define ORDER_HANDLER_HPP

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Waypoint pose in the robot map frame identified by `map_id`.
struct RobotPose
{
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
};

// Route waypoint and the optional speed limit for its incoming edge.
struct RouteWaypoint
{
    std::string node_id;
    RobotPose pose;
    std::optional<double> speed_limit;
};

// Builds a route order with the base node at sequenceId 0, edges on odd
// sequenceIds, and route nodes on even sequenceIds. Nodes are released and
// edges do not include trajectories.
//
// Preconditions:
// - `order_id`, `base_node_id`, `route`, and `map_id` are non-empty.
// - Poses and speed limits are finite and expressed in the specified map.
// - `order_update_id` follows the VDA5050 order-update sequence.
nlohmann::json build_route_order(int header_id, const std::string &order_id,
                                const std::string &manufacturer, const std::string &serial,
                                const std::string &base_node_id, const RobotPose &base,
                                const std::vector<RouteWaypoint> &route,
                                const std::string &map_id,
                                int order_update_id = 0);

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ORDER_HANDLER_HPP
