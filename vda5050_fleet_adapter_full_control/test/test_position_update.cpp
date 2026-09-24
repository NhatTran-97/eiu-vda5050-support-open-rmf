#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "vda5050_fleet_adapter_full_control/rmf/position_update.hpp"

using vda5050_fleet_adapter_full_control::rmf::choose_position_update;
using vda5050_fleet_adapter_full_control::rmf::PositionUpdate;
using vda5050_fleet_adapter_full_control::rmf::RouteTarget;

namespace {

constexpr double kMergeWaypoint = 0.1;
constexpr double kMergeLane = 0.3;

// A(0,0) -> B(4,0) -> C(4,4) on L1, lanes 0 and 1; D(0,0) on L2 with lane 2 from D to E(4,0).
class PositionUpdateTest : public ::testing::Test
{
protected:
    PositionUpdateTest()
    {
        graph.add_waypoint("L1", {0.0, 0.0});
        graph.add_waypoint("L1", {4.0, 0.0});
        graph.add_waypoint("L1", {4.0, 4.0});
        graph.add_waypoint("L2", {0.0, 0.0});
        graph.add_waypoint("L2", {4.0, 0.0});
        graph.add_lane(0, 1);
        graph.add_lane(1, 2);
        graph.add_lane(3, 4);
    }

    PositionUpdate at(double x, double y, const std::optional<RouteTarget> &target, const std::string &map = "L1") const
    {
        return choose_position_update(graph, map, {x, y, 0.0}, target, kMergeWaypoint, kMergeLane);
    }

    rmf_traffic::agv::Graph graph;
};

}  // namespace

TEST_F(PositionUpdateTest, WithoutARouteTheMapAndPositionAreReported)
{
    EXPECT_EQ(at(2.0, 0.0, std::nullopt).kind, PositionUpdate::Kind::map);
}

TEST_F(PositionUpdateTest, OnTheTargetWaypointTheWaypointIsReported)
{
    const auto update = at(4.05, 0.0, RouteTarget{1, {0}});
    EXPECT_EQ(update.kind, PositionUpdate::Kind::waypoint);
    EXPECT_EQ(update.waypoint, 1u);
}

TEST_F(PositionUpdateTest, OnTheWayToTheTargetItsApproachLaneIsReported)
{
    const auto update = at(2.0, 0.2, RouteTarget{1, {0}});
    EXPECT_EQ(update.kind, PositionUpdate::Kind::lanes);
    EXPECT_EQ(update.lanes, std::vector<std::size_t>{0});
}

TEST_F(PositionUpdateTest, OnlyTheApproachLanesTheRobotIsNearAreReported)
{
    // A target reached through two lanes, as RMF gives for waypoints merged along a straight line.
    const auto update = at(2.0, 0.0, RouteTarget{2, {0, 1}});
    EXPECT_EQ(update.kind, PositionUpdate::Kind::lanes);
    EXPECT_EQ(update.lanes, std::vector<std::size_t>{0});
}

TEST_F(PositionUpdateTest, ARobotOffEveryApproachLaneIsReportedByMap)
{
    EXPECT_EQ(at(2.0, 1.0, RouteTarget{1, {0}}).kind, PositionUpdate::Kind::map);
}

TEST_F(PositionUpdateTest, ATargetWithoutWaypointOrLanesIsReportedByMap)
{
    EXPECT_EQ(at(2.0, 0.0, RouteTarget{std::nullopt, {}}).kind, PositionUpdate::Kind::map);
}

TEST_F(PositionUpdateTest, WaypointsAndLanesOfAnotherMapAreNotUsed)
{
    EXPECT_EQ(at(4.0, 0.0, RouteTarget{1, {0}}, "L2").kind, PositionUpdate::Kind::map);
    EXPECT_EQ(at(2.0, 0.0, RouteTarget{4, {2}}, "L1").kind, PositionUpdate::Kind::map);
    EXPECT_EQ(at(2.0, 0.0, RouteTarget{4, {2}}, "L2").kind, PositionUpdate::Kind::lanes);
}

TEST_F(PositionUpdateTest, IndicesOutsideTheGraphAreIgnored)
{
    EXPECT_EQ(at(2.0, 0.0, RouteTarget{99, {99}}).kind, PositionUpdate::Kind::map);
    const auto update = at(2.0, 0.0, RouteTarget{99, {99, 0}});
    EXPECT_EQ(update.kind, PositionUpdate::Kind::lanes);
    EXPECT_EQ(update.lanes, std::vector<std::size_t>{0});
}
