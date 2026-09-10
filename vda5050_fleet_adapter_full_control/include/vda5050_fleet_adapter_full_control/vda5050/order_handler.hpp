#ifndef ORDER_HANDLER_HPP
#define ORDER_HANDLER_HPP

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// A resolved waypoint pose in the map frame identified by the order's
// map_id -- NOT a frame attached to the robot's body, and NOT RMF's world
// frame. The caller (Connector) resolves x/y/theta before calling in here.
struct RobotPose
{
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
};

// One stop along a route: where to go, and how fast the AGV may travel on
// the edge leading into it.
struct RouteWaypoint
{
    std::string node_id;
    RobotPose pose;
    std::optional<double> speed_limit;
};

// Builds a VDA5050 'order' covering a whole route: the base node the AGV is
// standing on at sequenceId 0, then one edge + node pair per waypoint --
// edges on odd sequenceIds, nodes on even, as VDA5050 requires.
//
// Every node is released. VDA5050's horizon exists so master control can
// plan further than it is willing to commit; RMF has already deconflicted
// the path it hands over, so there is nothing to hold back.
//
// No trajectory is set on any edge, so the path the AGV actually drives
// between two nodes is up to its own navigation stack.
//
// order_update_id: 0 for a brand-new order. A higher value alongside an
// order_id the AGV is already running asks it to stitch this route onto
// that one instead of starting over; whether the AGV accepts the stitch is
// its decision (VDA5050 6.6).
//
// Preconditions the caller must satisfy (not checked here):
// - base and every waypoint pose are expressed in the map named by map_id.
// - all poses and speed limits are finite.
// - order_id is non-empty -- passing "" would let make_order() generate a
//   UUID the caller never sees, breaking completion tracking.
// - route is non-empty; an empty route yields an order with a single node
//   and no edges, which the AGV would treat as "already there".
nlohmann::json build_route_order(
    int header_id, const std::string &order_id,
    const std::string &manufacturer, const std::string &serial,
    const std::string &base_node_id, const RobotPose &base,
    const std::vector<RouteWaypoint> &route,
    const std::string &map_id,
    int order_update_id = 0);

// Single-destination convenience wrapper over build_route_order(): the
// two-node, one-edge shape this adapter sent before it could express whole
// routes.
nlohmann::json build_navigate_order(
    int header_id, const std::string &order_id,
    const std::string &manufacturer, const std::string &serial,
    const std::string &base_node_id, const RobotPose &base,
    const std::string &dest_node_id, const RobotPose &dest,
    const std::string &map_id,
    std::optional<double> speed_limit = std::nullopt);

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ORDER_HANDLER_HPP
