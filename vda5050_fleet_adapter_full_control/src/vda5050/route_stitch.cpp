#include "vda5050_fleet_adapter_full_control/vda5050/route_stitch.hpp"

#include <cmath>

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

bool same_point(const RouteWaypoint &a, const RouteWaypoint &b, double tolerance)
{
    return a.node_id == b.node_id && std::hypot(a.pose.x - b.pose.x, a.pose.y - b.pose.y) <= tolerance;
}

}  // namespace

std::optional<StitchPlan> plan_stitch(const std::vector<RouteWaypoint> &current_route,
                                      std::size_t released_count, std::size_t traversed,
                                      const std::vector<RouteWaypoint> &new_route,
                                      double position_tolerance)
{
    if (new_route.empty() || released_count > current_route.size() || traversed > released_count)
    {
        return std::nullopt;
    }

    // Released route points the AGV has not reached yet.
    const std::size_t remaining = released_count - traversed;
    if (new_route.size() < remaining)
    {
        return std::nullopt;
    }
    for (std::size_t k = 0; k < remaining; ++k)
    {
        if (!same_point(current_route[traversed + k], new_route[k], position_tolerance))
        {
            return std::nullopt;
        }
    }

    StitchPlan plan;
    plan.route.assign(current_route.begin(), current_route.begin() + released_count);
    plan.route.insert(plan.route.end(), new_route.begin() + remaining, new_route.end());
    plan.consumed = traversed;
    plan.stitch_index = released_count;

    const std::size_t old_tail = current_route.size() - released_count;
    const std::size_t new_tail = new_route.size() - remaining;
    plan.unchanged = old_tail == new_tail;
    for (std::size_t k = 0; plan.unchanged && k < new_tail; ++k)
    {
        plan.unchanged = same_point(current_route[released_count + k], new_route[remaining + k],
                                    position_tolerance);
    }
    return plan;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
