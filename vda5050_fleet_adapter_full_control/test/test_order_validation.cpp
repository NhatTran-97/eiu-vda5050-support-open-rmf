#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "vda5050_fleet_adapter_full_control/vda5050/order_validation.hpp"

using namespace vda5050_fleet_adapter_full_control::vda5050;

namespace {

OrderShape shape(std::size_t nodes, std::size_t edges) {
  OrderShape s;
  s.node_count = nodes;
  s.edge_count = edges;
  s.map_ids = {"m"};
  s.poses = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
  return s;
}

}  // namespace

TEST(OrderValidationTest, AcceptsOrderWithoutFactsheet) {
  EXPECT_TRUE(check_order(shape(2, 1), std::nullopt, {}).empty());
}

TEST(OrderValidationTest, RejectsNonFinitePose) {
  auto s = shape(2, 1);
  s.poses[1][0] = std::numeric_limits<double>::quiet_NaN();
  const auto v = check_order(s, std::nullopt, {});
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].severity, Severity::hard);
  EXPECT_TRUE(has_hard_violation(v));
}

TEST(OrderValidationTest, RejectsOrderOverDeclaredArrayLimits) {
  ParsedFactsheet fs;
  fs.max_order_nodes = 3;
  fs.max_order_edges = 2;
  const auto v = check_order(shape(5, 4), fs, {});
  ASSERT_EQ(v.size(), 2u);
  EXPECT_TRUE(has_hard_violation(v));
  EXPECT_TRUE(check_order(shape(3, 2), fs, {}).empty());
}

TEST(OrderValidationTest, RejectsMapTheAgvDoesNotReport) {
  EXPECT_TRUE(has_hard_violation(check_order(shape(2, 1), std::nullopt, {"other"})));
  EXPECT_TRUE(check_order(shape(2, 1), std::nullopt, {"m", "other"}).empty());
  // An AGV that reports no maps gives nothing to compare against.
  EXPECT_TRUE(check_order(shape(2, 1), std::nullopt, {}).empty());
}

TEST(OrderValidationTest, OrderIntervalIsOnlyAWarning) {
  ParsedFactsheet fs;
  fs.min_order_interval = 2.0;
  auto s = shape(2, 1);
  s.seconds_since_last_order = 0.5;
  const auto v = check_order(s, fs, {});
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].severity, Severity::soft);
  EXPECT_FALSE(has_hard_violation(v));
}

TEST(OrderValidationTest, RejectsCustomActionMissingFromFactsheet) {
  ParsedFactsheet fs;
  fs.agv_actions["pick"] = {};
  EXPECT_FALSE(check_instant_action("pick", fs).has_value());
  const auto v = check_instant_action("drop", fs);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->severity, Severity::hard);
}

TEST(OrderValidationTest, CoreActionsAreNeverBlockedByFactsheet) {
  ParsedFactsheet fs;
  fs.agv_actions["pick"] = {};
  for (const char *core : {"cancelOrder", "startPause", "stopPause", "stateRequest",
                           "initPosition", "factsheetRequest"}) {
    const auto v = check_instant_action(core, fs);
    ASSERT_TRUE(v.has_value()) << core;
    EXPECT_EQ(v->severity, Severity::soft) << core;
  }
  EXPECT_FALSE(check_instant_action("drop", std::nullopt).has_value());
}
