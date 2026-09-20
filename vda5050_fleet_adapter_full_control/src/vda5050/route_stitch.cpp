#include "vda5050_fleet_adapter_full_control/vda5050/route_stitch.hpp"

#include <algorithm>
#include <cmath>

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

bool same_point(const RouteWaypoint &a, const RouteWaypoint &b, double tolerance)
{
    return a.node_id == b.node_id && std::hypot(a.pose.x - b.pose.x, a.pose.y - b.pose.y) <= tolerance;
}

bool at_position(const RouteWaypoint &a, const RouteWaypoint &b, double tolerance)
{
    return std::hypot(a.pose.x - b.pose.x, a.pose.y - b.pose.y) <= tolerance;
}

// Distance from point p to the segment a-b.
double distance_to_segment(const RobotPose &p, const RobotPose &a, const RobotPose &b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_sq = dx * dx + dy * dy;
    const double t = length_sq == 0.0
                         ? 0.0
                         : std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length_sq, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Whether the first `lead` points of the new route lie on the AGV's lane.
bool leading_on_lane(const std::vector<RouteWaypoint> &current_route, std::size_t traversed,
                     const std::vector<RouteWaypoint> &new_route, std::size_t lead, double lane_tolerance)
{
    const RobotPose &start = traversed > 0 ? current_route[traversed - 1].pose : new_route.front().pose;
    const RobotPose &next = current_route[traversed].pose;
    for (std::size_t k = 0; k < lead; ++k)
    {
        if (distance_to_segment(new_route[k].pose, start, next) > lane_tolerance)
        {
            return false;
        }
    }
    return true;
}

std::optional<StitchPlan> attach(const std::vector<RouteWaypoint> &current_route, std::size_t released_count, std::size_t traversed,
                                 const std::vector<RouteWaypoint> &new_route, std::size_t lead,
                                 double position_tolerance, double lane_tolerance)
{
    // Match the released, unreached points against the start of the new route.
    // Repeated turns may differ in number, and a point may lie on a straight hop.
    std::size_t n = lead;
    std::size_t k = traversed;
    while (k < released_count)
    {
        std::size_t group = 1;
        while (k + group < released_count && same_point(current_route[k], current_route[k + group], position_tolerance))
        {
            ++group;
        }
        if (n < new_route.size() && same_point(current_route[k], new_route[n], position_tolerance))
        {
            std::size_t used = 1;
            while (used < group && n + used < new_route.size() && same_point(current_route[k], new_route[n + used], position_tolerance))
            {
                ++used;
            }
            n += used;
        }
        else if (!(n > 0 && n < new_route.size() &&
                   distance_to_segment(current_route[k].pose, new_route[n - 1].pose, new_route[n].pose) <= lane_tolerance && !at_position(current_route[k], new_route[n], position_tolerance)))
        {
            // The new route neither stops at this point nor passes straight through it.
            return std::nullopt;
        }
        k += group;
    }
    const std::size_t matched = n - lead;
    const std::size_t remaining = released_count - traversed;

    StitchPlan plan;
    plan.route.assign(current_route.begin(), current_route.begin() + released_count);
    plan.route.insert(plan.route.end(), new_route.begin() + n, new_route.end());
    // Order points the new route does not repeat count as already passed.
    plan.consumed = traversed + (remaining - matched);
    plan.stitch_index = released_count;
    plan.leading = lead;

    const std::size_t old_tail = current_route.size() - released_count;
    const std::size_t new_tail = new_route.size() - n;
    plan.unchanged = old_tail == new_tail;
    for (std::size_t i = 0; plan.unchanged && i < new_tail; ++i)
    {
        plan.unchanged = same_point(current_route[released_count + i], new_route[n + i], position_tolerance);
    }
    return plan;
}

}  // namespace

std::optional<StitchPlan> plan_stitch(const std::vector<RouteWaypoint> &current_route, std::size_t released_count, std::size_t traversed,
                                      const std::vector<RouteWaypoint> &new_route, double position_tolerance, std::size_t max_leading, double lane_tolerance)
{
    if (new_route.empty() || released_count > current_route.size() || traversed > released_count)
    {
        return std::nullopt;
    }

    if (auto plan = attach(current_route, released_count, traversed, new_route, 0, position_tolerance, lane_tolerance))
    {
        return plan;
    }

    // No released part left, so no lane to skip along.
    if (traversed == released_count)
    {
        return std::nullopt;
    }
    for (std::size_t lead = 1; lead <= max_leading && lead < new_route.size(); ++lead)
    {
        if (!leading_on_lane(current_route, traversed, new_route, lead, lane_tolerance))
        {
            break;
        }
        if (auto plan = attach(current_route, released_count, traversed, new_route, lead, position_tolerance, lane_tolerance))
        {
            return plan;
        }
    }
    return std::nullopt;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
