#include "vda5050_fleet_adapter_full_control/rmf/position_update.hpp"

#include <algorithm>

namespace vda5050_fleet_adapter_full_control::rmf {

namespace {

// Distance from `point` to the segment between a lane's entry and exit waypoints.
double distance_to_lane(const rmf_traffic::agv::Graph &graph, const rmf_traffic::agv::Graph::Lane &lane,
                        const Eigen::Vector2d &point)
{
    const Eigen::Vector2d a = graph.get_waypoint(lane.entry().waypoint_index()).get_location();
    const Eigen::Vector2d b = graph.get_waypoint(lane.exit().waypoint_index()).get_location();
    const Eigen::Vector2d ab = b - a;
    const double length_sq = ab.squaredNorm();
    const double t = length_sq > 0.0 ? std::clamp((point - a).dot(ab) / length_sq, 0.0, 1.0) : 0.0;
    return (a + t * ab - point).norm();
}

}  // namespace

PositionUpdate choose_position_update(const rmf_traffic::agv::Graph &graph, const std::string &map_name,
                                      const Eigen::Vector3d &position, const std::optional<RouteTarget> &target,
                                      double merge_waypoint_m, double merge_lane_m)
{
    PositionUpdate update;
    if (!target.has_value())
    {
        return update;
    }
    const Eigen::Vector2d point = position.head<2>();

    if (target->waypoint.has_value() && *target->waypoint < graph.num_waypoints())
    {
        const auto &waypoint = graph.get_waypoint(*target->waypoint);
        if (waypoint.get_map_name() == map_name && (waypoint.get_location() - point).norm() <= merge_waypoint_m)
        {
            update.kind = PositionUpdate::Kind::waypoint;
            update.waypoint = *target->waypoint;
            return update;
        }
    }

    for (const std::size_t index : target->approach_lanes)
    {
        if (index >= graph.num_lanes())
        {
            continue;
        }
        const auto &lane = graph.get_lane(index);
        if (graph.get_waypoint(lane.entry().waypoint_index()).get_map_name() == map_name &&
            distance_to_lane(graph, lane, point) <= merge_lane_m)
        {
            update.lanes.push_back(index);
        }
    }
    if (!update.lanes.empty())
    {
        update.kind = PositionUpdate::Kind::lanes;
    }
    return update;
}

}  // namespace vda5050_fleet_adapter_full_control::rmf
