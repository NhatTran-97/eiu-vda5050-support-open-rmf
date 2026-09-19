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
};

// Attach a replanned route to the live order at the end of its released base.
// The AGV keeps driving the released part, so the new route must repeat it exactly.
// Returns nullopt when it does not, and the caller sends a replacement order instead.
std::optional<StitchPlan> plan_stitch(const std::vector<RouteWaypoint> &current_route,
                                      std::size_t released_count, std::size_t traversed,
                                      const std::vector<RouteWaypoint> &new_route,
                                      double position_tolerance = 0.10);

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ROUTE_STITCH_HPP
