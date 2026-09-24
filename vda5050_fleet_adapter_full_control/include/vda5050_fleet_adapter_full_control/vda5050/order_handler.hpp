#ifndef ORDER_HANDLER_HPP
#define ORDER_HANDLER_HPP

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Waypoint pose in robot map coordinates.
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

// How far the AGV may stop from each node of an order and still count it as reached.
struct NodeDeviation
{
    double xy_m = 0.5;
    double theta_rad = 3.14;
};

// Build a route order with alternating node and edge IDs and a released horizon.
// stitch_index > 0 builds an update that starts at that node and keeps its sequence IDs.
nlohmann::json build_route_order(int header_id, const std::string &order_id,
                                const std::string &manufacturer, const std::string &serial, const std::string &base_node_id, const RobotPose &base,
                                const std::vector<RouteWaypoint> &route,  const std::string &map_id,
                                int order_update_id = 0,  std::optional<std::size_t> released_count = std::nullopt,  std::size_t stitch_index = 0,
                                const NodeDeviation &deviation = {});

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ORDER_HANDLER_HPP
