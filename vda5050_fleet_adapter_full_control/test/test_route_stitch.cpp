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
  // Same id but a different place is not the same point.
  EXPECT_FALSE(plan_stitch(kOld, 2, 0, {wp("A", 1), wp("B", 7)}).has_value());
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

TEST(RouteStitchTest, RejectsInconsistentInput) {
  EXPECT_FALSE(plan_stitch(kOld, 2, 0, {}).has_value());
  EXPECT_FALSE(plan_stitch(kOld, 9, 0, kOld).has_value());
  EXPECT_FALSE(plan_stitch(kOld, 2, 3, kOld).has_value());
}
