#include <gtest/gtest.h>

#include <string>

#include "vda5050_fleet_adapter_full_control/vda5050/route_stitch.hpp"

using namespace vda5050_fleet_adapter_full_control::vda5050;

namespace {

RouteWaypoint wp(const std::string &id, double x) {
  RouteWaypoint w;
  w.node_id = id;
  w.pose = {x, 0.0, 0.0};
  return w;
}

std::vector<std::string> ids(const std::vector<RouteWaypoint> &route) {
  std::vector<std::string> out;
  for (const auto &w : route) out.push_back(w.node_id);
  return out;
}

// A B C D E
const std::vector<RouteWaypoint> kOld = {wp("A", 1), wp("B", 2), wp("C", 3), wp("D", 4), wp("E", 5)};

}  // namespace

TEST(RouteStitchTest, ReplacesUnreleasedTail) {
  // A, B released and none reached; the replan keeps A, B and changes the rest.
  const auto plan = plan_stitch(kOld, 2, 0, {wp("A", 1), wp("B", 2), wp("X", 9), wp("Y", 10)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "B", "X", "Y"}));
  EXPECT_EQ(plan->stitch_index, 2u);
  EXPECT_EQ(plan->consumed, 0u);
  EXPECT_FALSE(plan->unchanged);
}

TEST(RouteStitchTest, ReplanStartsAfterTheReachedPoints) {
  // The AGV passed A; the new path starts at the next point.
  const auto plan = plan_stitch(kOld, 3, 1, {wp("B", 2), wp("C", 3), wp("X", 9)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "B", "C", "X"}));
  EXPECT_EQ(plan->consumed, 1u);
  EXPECT_EQ(plan->stitch_index, 3u);
}

TEST(RouteStitchTest, RejectsRouteThatDivergesInsideTheReleasedBase) {
  EXPECT_FALSE(plan_stitch(kOld, 3, 0, {wp("A", 1), wp("Z", 2), wp("C", 3)}).has_value());
  // Same id but a place off the lane is not the same point.
  RouteWaypoint elsewhere = wp("B", 7);
  elsewhere.pose.y = 3.0;
  EXPECT_FALSE(plan_stitch(kOld, 2, 0, {wp("A", 1), elsewhere}).has_value());
}

TEST(RouteStitchTest, RejectsRouteShorterThanTheReleasedBase) {
  EXPECT_FALSE(plan_stitch(kOld, 3, 0, {wp("A", 1), wp("B", 2)}).has_value());
}

TEST(RouteStitchTest, ReportsIdenticalTailAsUnchanged) {
  const auto plan = plan_stitch(kOld, 2, 0, kOld);
  ASSERT_TRUE(plan.has_value());
  EXPECT_TRUE(plan->unchanged);
  EXPECT_EQ(plan->route.size(), kOld.size());
}

TEST(RouteStitchTest, TruncatingTheTailIsAChange) {
  const auto plan = plan_stitch(kOld, 2, 0, {wp("A", 1), wp("B", 2)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_FALSE(plan->unchanged);
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "B"}));
}

TEST(RouteStitchTest, AttachesAtTheLastReachedPointWhileWaitingForRelease) {
  // Base fully consumed: the AGV waits at B and the whole new path is the tail.
  const auto plan = plan_stitch(kOld, 2, 2, {wp("X", 9), wp("Y", 10)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "B", "X", "Y"}));
  EXPECT_EQ(plan->consumed, 2u);
  EXPECT_EQ(plan->stitch_index, 2u);
}

TEST(RouteStitchTest, SkipsLeadingPointsOnTheLaneToTheNextNode) {
  // The new path starts on the lane to C and adds B.
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("D", 6)};
  const auto plan = plan_stitch(old_route, 2, 1, {wp("1.50_0.00", 1.5), wp("B", 2), wp("C", 4), wp("X", 9)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->leading, 2u);
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "C", "X"}));
  EXPECT_EQ(plan->consumed, 1u);
  EXPECT_EQ(plan->stitch_index, 2u);
}

TEST(RouteStitchTest, DoesNotSkipLeadingPointsOffTheLane) {
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("D", 6)};
  RouteWaypoint detour = wp("Q", 2);
  detour.pose.y = 3.0;
  EXPECT_FALSE(plan_stitch(old_route, 2, 1, {detour, wp("C", 4), wp("X", 9)}).has_value());
}

TEST(RouteStitchTest, SkipsAtMostMaxLeadingPoints) {
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 8), wp("D", 9)};
  const std::vector<RouteWaypoint> new_route = {wp("p1", 1), wp("p2", 2), wp("p3", 3), wp("p4", 4), wp("C", 8), wp("X", 12)};
  EXPECT_FALSE(plan_stitch(old_route, 2, 1, new_route, 0.10, 3).has_value());
  EXPECT_TRUE(plan_stitch(old_route, 2, 1, new_route, 0.10, 4).has_value());
}

TEST(RouteStitchTest, PrefersNoSkippedPointsWhenTheRouteMatchesDirectly) {
  const auto plan = plan_stitch(kOld, 2, 0, {wp("A", 1), wp("B", 2), wp("X", 9)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->leading, 0u);
}

TEST(RouteStitchTest, AbsorbsTurnsInPlaceThatTheNewRouteDoesNotRepeat) {
  // The order has a turn at C; the new path passes C once.
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("C", 4), wp("D", 6), wp("E", 8)};
  const auto plan = plan_stitch(old_route, 3, 1, {wp("B", 2), wp("C", 4), wp("X", 9)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->leading, 1u);
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "C", "C", "X"}));
  EXPECT_EQ(plan->stitch_index, 3u);
  // Two order points (C, C) stand for the new route's single C.
  EXPECT_EQ(plan->consumed, 2u);
}

TEST(RouteStitchTest, KeepsTurnsInPlaceBeyondTheReleasedPointsInTheTail) {
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("D", 6)};
  const auto plan = plan_stitch(old_route, 2, 1, {wp("C", 4), wp("C", 4), wp("X", 9)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "C", "C", "X"}));
  EXPECT_EQ(plan->consumed, 1u);
}

TEST(RouteStitchTest, RejectsRouteThatStopsBeforeTheRepeatedPointIsCovered) {
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("C", 4), wp("D", 6)};
  EXPECT_FALSE(plan_stitch(old_route, 3, 1, {wp("X", 9)}).has_value());
}

TEST(RouteStitchTest, AcceptsReleasedPointThatTheNewRouteDrivesStraightThrough) {
  // C lies on the straight hop B -> F.
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("D", 6)};
  const auto plan = plan_stitch(old_route, 2, 1, {wp("B", 2), wp("F", 9), wp("G", 12)});
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->leading, 1u);
  EXPECT_EQ(ids(plan->route), (std::vector<std::string>{"A", "C", "F", "G"}));
  EXPECT_EQ(plan->stitch_index, 2u);
  EXPECT_EQ(plan->consumed, 2u);
  EXPECT_FALSE(plan->unchanged);
}

TEST(RouteStitchTest, RejectsReleasedPointThatTheNewRouteMissesByALane) {
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 4), wp("D", 6)};
  RouteWaypoint away = wp("F", 9);
  away.pose.y = 3.0;
  EXPECT_FALSE(plan_stitch(old_route, 2, 1, {wp("B", 2), away, wp("G", 12)}).has_value());
}

TEST(RouteStitchTest, RejectsReleasedPointBeyondTheEndOfTheHop) {
  const std::vector<RouteWaypoint> old_route = {wp("A", 0), wp("C", 12), wp("D", 14)};
  EXPECT_FALSE(plan_stitch(old_route, 2, 1, {wp("B", 2), wp("F", 9), wp("G", 11)}).has_value());
}

TEST(RouteStitchTest, RejectsInconsistentInput) {
  EXPECT_FALSE(plan_stitch(kOld, 2, 0, {}).has_value());
  EXPECT_FALSE(plan_stitch(kOld, 9, 0, kOld).has_value());
  EXPECT_FALSE(plan_stitch(kOld, 2, 3, kOld).has_value());
}
