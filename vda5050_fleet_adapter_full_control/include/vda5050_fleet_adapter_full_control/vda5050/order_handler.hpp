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

// Builds a route order: base node at sequenceId 0, edges on odd ids, route
// nodes on even ids (no trajectories).
//
// released_count: how many route points (from the base, always released)
// go out now; nullopt releases everything, a smaller value leaves the rest
// as horizon for a later order update on the same order_id.
//
// Requires non-empty order_id/base_node_id/route/map_id, finite poses and
// speed limits, and order_update_id following VDA5050's update sequence.
nlohmann::json build_route_order(int header_id, const std::string &order_id,
                                const std::string &manufacturer, const std::string &serial,
                                const std::string &base_node_id, const RobotPose &base,
                                const std::vector<RouteWaypoint> &route,
                                const std::string &map_id,
                                int order_update_id = 0,
                                std::optional<std::size_t> released_count = std::nullopt);

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ORDER_HANDLER_HPP
