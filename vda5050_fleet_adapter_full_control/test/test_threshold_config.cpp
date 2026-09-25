#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/link_policy.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/route_policy.hpp"

using namespace vda5050_fleet_adapter_full_control::core;
using vda5050_fleet_adapter_full_control::rmf::LinkPolicy;
using vda5050_fleet_adapter_full_control::rmf::RoutePolicy;

namespace {

// Write a config file whose vda5050 section ends with `extra` and return its path.
std::string write_config(const std::string &extra)
{
    const std::string path = ::testing::TempDir() + "threshold_config_test.yaml";
    std::ofstream(path) << "vda5050:\n  interface_name: test\n" << extra;
    return path;
}

// Whether loading the config fails with an error that names `text`.
bool rejected_naming(const std::string &extra, const std::string &text)
{
    try
    {
        Config config(write_config(extra));
    }
    catch (const std::runtime_error &e)
    {
        return std::string(e.what()).find(text) != std::string::npos;
    }
    return false;
}

}  // namespace

TEST(ThresholdConfigTest, DefaultsApplyWhenTheKeysAreAbsent)
{
    const Config config(write_config(""));
    const LinkPolicy link;
    const RoutePolicy route;
    EXPECT_DOUBLE_EQ(config.link_policy().state_timeout_s, link.state_timeout_s);
    EXPECT_DOUBLE_EQ(config.link_policy().order_stuck_timeout_s, link.order_stuck_timeout_s);
    EXPECT_DOUBLE_EQ(config.link_policy().factsheet_first_wait_s, link.factsheet_first_wait_s);
    EXPECT_DOUBLE_EQ(config.link_policy().factsheet_retry_wait_s, link.factsheet_retry_wait_s);
    EXPECT_EQ(config.link_policy().factsheet_request_attempts, link.factsheet_request_attempts);
    EXPECT_DOUBLE_EQ(config.route_policy().waypoint_reached_m, route.waypoint_reached_m);
    EXPECT_DOUBLE_EQ(config.route_policy().same_pose_m, route.same_pose_m);
    EXPECT_DOUBLE_EQ(config.route_policy().same_pose_rad, route.same_pose_rad);
    EXPECT_DOUBLE_EQ(config.route_policy().usable_speed_mps, route.usable_speed_mps);
    EXPECT_DOUBLE_EQ(config.route_policy().early_arrival_warn_s, route.early_arrival_warn_s);
    EXPECT_DOUBLE_EQ(config.route_policy().traffic_pause_timeout_s, route.traffic_pause_timeout_s);
    EXPECT_DOUBLE_EQ(config.registration().timeout_s, RegistrationConfig{}.timeout_s);
    EXPECT_DOUBLE_EQ(config.link_policy().offline_state_intervals, link.offline_state_intervals);
    EXPECT_DOUBLE_EQ(config.route_policy().timed_release_max_delay_s, route.timed_release_max_delay_s);
    EXPECT_DOUBLE_EQ(config.route_policy().replan_after_s, link.order_stuck_timeout_s);
    EXPECT_DOUBLE_EQ(config.node_deviation().xy_m, 0.5);
    EXPECT_DOUBLE_EQ(config.node_deviation().theta_rad, 3.14);
    EXPECT_DOUBLE_EQ(config.init_position_timeout_s(), 10.0);
}

TEST(ThresholdConfigTest, ThePolicyDefaultsAreTheValuesTheAdapterAlwaysUsed)
{
    const LinkPolicy link;
    const RoutePolicy route;
    EXPECT_DOUBLE_EQ(link.state_timeout_s, 10.0);
    EXPECT_DOUBLE_EQ(link.order_stuck_timeout_s, 15.0);
    EXPECT_DOUBLE_EQ(link.factsheet_first_wait_s, 5.0);
    EXPECT_DOUBLE_EQ(link.factsheet_retry_wait_s, 20.0);
    EXPECT_EQ(link.factsheet_request_attempts, 3);
    EXPECT_DOUBLE_EQ(route.waypoint_reached_m, 0.5);
    EXPECT_DOUBLE_EQ(route.same_pose_m, 0.05);
    EXPECT_DOUBLE_EQ(route.same_pose_rad, 0.05);
    EXPECT_DOUBLE_EQ(route.usable_speed_mps, 0.05);
    EXPECT_DOUBLE_EQ(route.early_arrival_warn_s, 2.0);
    EXPECT_DOUBLE_EQ(route.traffic_pause_timeout_s, 10.0);
    EXPECT_DOUBLE_EQ(route.timed_release_max_delay_s, 0.0);
    EXPECT_DOUBLE_EQ(link.offline_state_intervals, 2.0);
    EXPECT_DOUBLE_EQ(link.order_ack_timeout_s, 5.0);
    EXPECT_EQ(link.order_resend_attempts, 2);
    EXPECT_TRUE(link.cancel_unknown_orders);
    EXPECT_DOUBLE_EQ(RegistrationConfig{}.timeout_s, 30.0);
}

TEST(ThresholdConfigTest, EveryKeyIsRead)
{
    const Config config(write_config(
        "  state_timeout_s: 20\n  order_stuck_timeout_s: 30.5\n  factsheet_first_wait_s: 1\n  factsheet_retry_wait_s: 2\n"
        "  factsheet_request_attempts: 7\n  waypoint_reached_m: 0.25\n  same_pose_m: 0.02\n  same_pose_rad: 0.1\n"
        "  usable_speed_mps: 0.08\n  early_arrival_warn_s: 4\n  traffic_pause_timeout_s: 12\n"
        "  offline_state_intervals: 3\n  timed_release_max_delay_s: 25\n  node_deviation_xy_m: 0.3\n"
        "  node_deviation_theta_rad: 0.5\n  init_position_timeout_s: 20\n"
        "  order_ack_timeout_s: 2.5\n  order_resend_attempts: 4\n  cancel_unknown_orders: false\n"
        "  registration:\n    timeout_s: 45\n"));
    EXPECT_DOUBLE_EQ(config.link_policy().state_timeout_s, 20.0);
    EXPECT_DOUBLE_EQ(config.link_policy().order_stuck_timeout_s, 30.5);
    EXPECT_DOUBLE_EQ(config.link_policy().factsheet_first_wait_s, 1.0);
    EXPECT_DOUBLE_EQ(config.link_policy().factsheet_retry_wait_s, 2.0);
    EXPECT_EQ(config.link_policy().factsheet_request_attempts, 7);
    EXPECT_DOUBLE_EQ(config.route_policy().waypoint_reached_m, 0.25);
    EXPECT_DOUBLE_EQ(config.route_policy().same_pose_m, 0.02);
    EXPECT_DOUBLE_EQ(config.route_policy().same_pose_rad, 0.1);
    EXPECT_DOUBLE_EQ(config.route_policy().usable_speed_mps, 0.08);
    EXPECT_DOUBLE_EQ(config.route_policy().early_arrival_warn_s, 4.0);
    EXPECT_DOUBLE_EQ(config.route_policy().traffic_pause_timeout_s, 12.0);
    EXPECT_DOUBLE_EQ(config.registration().timeout_s, 45.0);
    EXPECT_DOUBLE_EQ(config.link_policy().offline_state_intervals, 3.0);
    EXPECT_DOUBLE_EQ(config.route_policy().timed_release_max_delay_s, 25.0);
    EXPECT_DOUBLE_EQ(config.route_policy().replan_after_s, 30.5);
    EXPECT_DOUBLE_EQ(config.node_deviation().xy_m, 0.3);
    EXPECT_DOUBLE_EQ(config.node_deviation().theta_rad, 0.5);
    EXPECT_DOUBLE_EQ(config.init_position_timeout_s(), 20.0);
    EXPECT_DOUBLE_EQ(config.link_policy().order_ack_timeout_s, 2.5);
    EXPECT_EQ(config.link_policy().order_resend_attempts, 4);
    EXPECT_FALSE(config.link_policy().cancel_unknown_orders);
}

TEST(ThresholdConfigTest, ANullValueKeepsTheDefault)
{
    const Config config(write_config("  state_timeout_s: null\n  waypoint_reached_m: null\n"));
    EXPECT_DOUBLE_EQ(config.link_policy().state_timeout_s, LinkPolicy{}.state_timeout_s);
    EXPECT_DOUBLE_EQ(config.route_policy().waypoint_reached_m, RoutePolicy{}.waypoint_reached_m);
}

TEST(ThresholdConfigTest, ValuesOutOfRangeAreRejectedAtStartup)
{
    const std::vector<std::string> bad = {
        "state_timeout_s: 0.5",          "state_timeout_s: 3601",      "state_timeout_s: soon",
        "order_stuck_timeout_s: 0",      "order_stuck_timeout_s: 4000",
        "factsheet_first_wait_s: -1",    "factsheet_retry_wait_s: 0",  "factsheet_request_attempts: 11",
        "factsheet_request_attempts: -1", "factsheet_request_attempts: 1.5",
        "waypoint_reached_m: 0",         "waypoint_reached_m: 11",
        "same_pose_m: 0",                "same_pose_m: 2",
        "same_pose_rad: 0",              "same_pose_rad: 1.5",
        "usable_speed_mps: -0.1",        "usable_speed_mps: 2",
        "early_arrival_warn_s: -1",      "traffic_pause_timeout_s: -1", "traffic_pause_timeout_s: 3601",
        "waypoint_reached_m: .nan",      "state_timeout_s: .inf",
        "offline_state_intervals: 0.5",  "offline_state_intervals: 101",
        "timed_release_max_delay_s: -1", "timed_release_max_delay_s: 3601",
        "node_deviation_xy_m: 0",        "node_deviation_theta_rad: 0", "node_deviation_theta_rad: 4",
        "init_position_timeout_s: 0",    "init_position_timeout_s: 601",
        "order_ack_timeout_s: 0",        "order_ack_timeout_s: 3601",  "order_resend_attempts: -1",
        "order_resend_attempts: 11",     "order_resend_attempts: 1.5", "cancel_unknown_orders: maybe",
    };
    for (const auto &line : bad)
    {
        const std::string key = line.substr(0, line.find(':'));
        EXPECT_TRUE(rejected_naming("  " + line + "\n", "vda5050." + key)) << line;
    }
}

TEST(ThresholdConfigTest, TheRegistrationTimeoutIsValidated)
{
    for (const char *line : {"timeout_s: 0.5", "timeout_s: 601", "timeout_s: never"})
    {
        EXPECT_TRUE(rejected_naming(std::string("  registration:\n    ") + line + "\n", "vda5050.registration.timeout_s")) << line;
    }
}

TEST(ThresholdConfigTest, TheBoundsOfARangeAreAccepted)
{
    EXPECT_NO_THROW(Config(write_config("  state_timeout_s: 1\n  traffic_pause_timeout_s: 0\n  factsheet_request_attempts: 0\n")));
    EXPECT_NO_THROW(Config(write_config("  state_timeout_s: 3600\n  factsheet_request_attempts: 10\n  same_pose_rad: 1\n")));
}

TEST(ThresholdConfigTest, TheMetricsPeriodDefaultsToAMinuteAndCanBeTurnedOff)
{
    EXPECT_DOUBLE_EQ(Config(write_config("")).metrics_period_s(), 60.0);
    EXPECT_DOUBLE_EQ(Config(write_config("  metrics_period_s: 5\n")).metrics_period_s(), 5.0);
    EXPECT_DOUBLE_EQ(Config(write_config("  metrics_period_s: 0\n")).metrics_period_s(), 0.0);
    for (const char *line : {"metrics_period_s: -1", "metrics_period_s: 3601", "metrics_period_s: often"})
    {
        EXPECT_TRUE(rejected_naming(std::string("  ") + line + "\n", "vda5050.metrics_period_s")) << line;
    }
}
