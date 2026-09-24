#ifndef POSITION_UPDATE_HPP
#define POSITION_UPDATE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Geometry>
#include <rmf_traffic/agv/Graph.hpp>

namespace vda5050_fleet_adapter_full_control::rmf {

// The route point a robot is heading for, as RMF planned it.
struct RouteTarget
{
    std::optional<std::size_t> waypoint;
    std::vector<std::size_t> approach_lanes;
};

// How a robot's position is reported to RMF.
struct PositionUpdate
{
    enum class Kind
    {
        waypoint,
        lanes,
        map,
    };
    Kind kind = Kind::map;
    std::size_t waypoint = 0;
    std::vector<std::size_t> lanes;
};

// On the target waypoint, on the lanes leading to it that the robot is near, or else by map and position alone.
PositionUpdate choose_position_update(const rmf_traffic::agv::Graph &graph, const std::string &map_name,
                                      const Eigen::Vector3d &position, const std::optional<RouteTarget> &target,
                                      double merge_waypoint_m, double merge_lane_m);

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // POSITION_UPDATE_HPP
