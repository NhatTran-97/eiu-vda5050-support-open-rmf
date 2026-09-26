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
    // Time after a command could not be sent before RMF is asked to replan.
    double replan_after_s = 15.0;
    // Delay RMF tolerates before interrupting a robot while waypoint timing is honored; zero means no limit.
    double timed_release_max_delay_s = 0.0;
    // Distances within which a robot counts as on a waypoint or on a lane when its position goes to RMF.
    double merge_waypoint_m = 1e-3;
    double merge_lane_m = 0.3;
    // Cap edge maxSpeed at the fleet speed.
    bool cap_speed_to_fleet = false;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // ROUTE_POLICY_HPP
