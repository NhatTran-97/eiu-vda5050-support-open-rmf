#ifndef ROUTE_STITCH_HPP
#define ROUTE_STITCH_HPP

#include <cstddef>
#include <optional>
#include <vector>

#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

// How a replanned route continues the order the AGV is already executing.
struct StitchPlan
{
    // Released route points followed by the new tail.
    std::vector<RouteWaypoint> route;
    // Route points the AGV had passed before the new path starts.
    std::size_t consumed = 0;
    // Node index of the base end, where the update attaches.
    std::size_t stitch_index = 0;
    // True when the new tail equals the tail already in the order.
    bool unchanged = false;
    // Leading points of the new route the order already covers.
    std::size_t leading = 0;
};

// Attach a replanned route to the live order after its released base.
// The new route must repeat the released part; up to max_leading points on the AGV's lane may be skipped.
// Returns nullopt when it does not.
std::optional<StitchPlan> plan_stitch(const std::vector<RouteWaypoint> &current_route,
                                      std::size_t released_count, std::size_t traversed,
                                      const std::vector<RouteWaypoint> &new_route,
                                      double position_tolerance = 0.10,
                                      std::size_t max_leading = 3,
                                      double lane_tolerance = 0.15);

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ROUTE_STITCH_HPP
