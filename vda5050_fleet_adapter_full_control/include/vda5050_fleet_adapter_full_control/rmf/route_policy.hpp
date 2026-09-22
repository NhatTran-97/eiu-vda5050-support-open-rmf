#ifndef ROUTE_POLICY_HPP
#define ROUTE_POLICY_HPP

namespace vda5050_fleet_adapter_full_control::rmf {

// Distances, times and speeds a command handle uses while following a route.
struct RoutePolicy
{
    // Distance to a waypoint within which it counts as reached when the AGV reports no nodeId.
    double waypoint_reached_m = 0.5;
    // Poses closer than this in position and heading count as the same waypoint.
    double same_pose_m = 0.05;
    double same_pose_rad = 0.05;
    // Measured speeds below this are replaced by the fleet's nominal speed in estimates.
    double usable_speed_mps = 0.05;
    // Arrival ahead of the plan by more than this is logged.
    double early_arrival_warn_s = 2.0;
    // Time a traffic hold may last before it becomes a cancellation.
    double traffic_pause_timeout_s = 10.0;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // ROUTE_POLICY_HPP
