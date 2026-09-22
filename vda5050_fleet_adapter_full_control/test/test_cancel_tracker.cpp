#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/cancel_tracker.hpp"

using namespace vda5050_fleet_adapter_full_control::vda5050;
using std::chrono::seconds;
using Outcome = CancelTracker::Outcome;

namespace {

const CancelTracker::TimePoint kSent{std::chrono::hours(1)};

// A state that reports `order` as active (with pending nodes) or drained, and the given action states.
ParsedState state(const std::string &order, bool active, const nlohmann::json &actions = nlohmann::json::array(), bool driving = false)
{
    nlohmann::json raw = {{"orderId", order}, {"driving", driving}, {"actionStates", actions},
                          {"nodeStates", active ? nlohmann::json::array({{{"nodeId", "n1"}}}) : nlohmann::json::array()},
                          {"edgeStates", nlohmann::json::array()}};
    return ParsedState(raw);
}

nlohmann::json action(const std::string &id, const std::string &status, const std::string &description = "")
{
    return {{"actionId", id}, {"actionType", "cancelOrder"}, {"actionStatus", status}, {"resultDescription", description}};
}

CancelTracker tracker_for(const std::string &order = "order-1")
{
    CancelTracker tracker;
    tracker.sent("cancel-1", order, kSent);
    return tracker;
}

const CancelPolicy kPolicy{seconds(5), 3};

}  // namespace

TEST(CancelTrackerTest, NothingIsJudgedWithoutAPendingCancel)
{
    CancelTracker tracker;
    EXPECT_FALSE(tracker.pending());
    EXPECT_EQ(tracker.assess(state("order-1", true), kSent + seconds(9), kSent + seconds(9), kPolicy).outcome, Outcome::none);
}

TEST(CancelTrackerTest, AFinishedCancelEndsTheTracking)
{
    auto tracker = tracker_for();
    const auto verdict = tracker.assess(state("order-1", true, nlohmann::json::array({action("cancel-1", "FINISHED", "Order cancelled")})), kSent + seconds(1), kSent + seconds(1), kPolicy);
    EXPECT_EQ(verdict.outcome, Outcome::finished);
    EXPECT_EQ(verdict.detail, "Order cancelled");
    EXPECT_FALSE(tracker.pending());
}

TEST(CancelTrackerTest, AFailedCancelIsReportedWithTheAgvsReason)
{
    auto tracker = tracker_for();
    const auto verdict = tracker.assess(state("", false, nlohmann::json::array({action("cancel-1", "FAILED", "No active order to cancel")})), kSent + seconds(1), kSent + seconds(1), kPolicy);
    EXPECT_EQ(verdict.outcome, Outcome::failed);
    EXPECT_EQ(verdict.detail, "No active order to cancel");
    EXPECT_FALSE(tracker.pending());
}

TEST(CancelTrackerTest, AStateOlderThanTheCancelSaysNothing)
{
    auto tracker = tracker_for();
    EXPECT_EQ(tracker.assess(state("", false), kSent - seconds(1), kSent + seconds(30), kPolicy).outcome, Outcome::none);
    EXPECT_TRUE(tracker.pending());
}

TEST(CancelTrackerTest, AnOrderTheAgvNoLongerReportsEndsTheTracking)
{
    auto drained = tracker_for();
    EXPECT_EQ(drained.assess(state("order-1", false), kSent + seconds(1), kSent + seconds(1), kPolicy).outcome, Outcome::order_gone);
    EXPECT_FALSE(drained.pending());

    auto replaced = tracker_for();
    EXPECT_EQ(replaced.assess(state("order-2", true), kSent + seconds(1), kSent + seconds(1), kPolicy).outcome, Outcome::order_gone);

    auto idle = tracker_for();
    EXPECT_EQ(idle.assess(state("", false), kSent + seconds(1), kSent + seconds(1), kPolicy).outcome, Outcome::order_gone);
}

TEST(CancelTrackerTest, ADrivingAgvStillHasTheOrder)
{
    auto tracker = tracker_for();
    EXPECT_EQ(tracker.assess(state("order-1", false, nlohmann::json::array(), true), kSent + seconds(1), kSent + seconds(1), kPolicy).outcome, Outcome::none);
    EXPECT_TRUE(tracker.pending());
}

TEST(CancelTrackerTest, AnUnansweredCancelWaitsForTheTimeoutThenAsksForAResend)
{
    auto tracker = tracker_for();
    EXPECT_EQ(tracker.assess(state("order-1", true), kSent + seconds(4), kSent + seconds(4), kPolicy).outcome, Outcome::none);
    const auto verdict = tracker.assess(state("order-1", true), kSent + seconds(5), kSent + seconds(5), kPolicy);
    EXPECT_EQ(verdict.outcome, Outcome::resend);
    EXPECT_EQ(verdict.detail, "order-1");
    EXPECT_TRUE(tracker.pending());
}

TEST(CancelTrackerTest, AResendRestartsTheWaitAndCountsTheAttempt)
{
    auto tracker = tracker_for();
    EXPECT_EQ(tracker.attempts(), 1);
    tracker.resent("cancel-2", kSent + seconds(5));
    EXPECT_EQ(tracker.attempts(), 2);
    EXPECT_EQ(tracker.assess(state("order-1", true), kSent + seconds(7), kSent + seconds(7), kPolicy).outcome, Outcome::none);
    EXPECT_EQ(tracker.assess(state("order-1", true, nlohmann::json::array({action("cancel-2", "FINISHED")})), kSent + seconds(8), kSent + seconds(8), kPolicy).outcome, Outcome::finished);
}

TEST(CancelTrackerTest, TheOldActionIdIsNoLongerWatchedAfterAResend)
{
    auto tracker = tracker_for();
    tracker.resent("cancel-2", kSent + seconds(5));
    const auto verdict = tracker.assess(state("order-1", true, nlohmann::json::array({action("cancel-1", "FINISHED")})), kSent + seconds(6), kSent + seconds(6), kPolicy);
    EXPECT_EQ(verdict.outcome, Outcome::none);
}

TEST(CancelTrackerTest, AfterTheLastAttemptAnUnansweredCancelIsGivenUp)
{
    auto tracker = tracker_for();
    tracker.resent("cancel-2", kSent + seconds(5));
    tracker.resent("cancel-3", kSent + seconds(10));
    const auto verdict = tracker.assess(state("order-1", true), kSent + seconds(15), kSent + seconds(15), kPolicy);
    EXPECT_EQ(verdict.outcome, Outcome::unanswered);
    EXPECT_EQ(verdict.detail, "order-1");
    EXPECT_FALSE(tracker.pending());
}

TEST(CancelTrackerTest, APolicyOfOneAttemptNeverAsksForAResend)
{
    auto tracker = tracker_for();
    EXPECT_EQ(tracker.assess(state("order-1", true), kSent + seconds(5), kSent + seconds(5), CancelPolicy{seconds(5), 1}).outcome, Outcome::unanswered);
}

TEST(CancelTrackerTest, AnAcknowledgedCancelThatKeepsRunningIsReportedOnce)
{
    auto tracker = tracker_for();
    const auto running = state("order-1", true, nlohmann::json::array({action("cancel-1", "RUNNING")}));
    EXPECT_EQ(tracker.assess(running, kSent + seconds(2), kSent + seconds(2), kPolicy).outcome, Outcome::none);
    const auto slow = tracker.assess(running, kSent + seconds(5), kSent + seconds(5), kPolicy);
    EXPECT_EQ(slow.outcome, Outcome::still_running);
    EXPECT_EQ(slow.detail, "RUNNING");
    EXPECT_EQ(tracker.assess(running, kSent + seconds(9), kSent + seconds(9), kPolicy).outcome, Outcome::none);
    EXPECT_TRUE(tracker.pending());
    EXPECT_EQ(tracker.assess(state("order-1", true, nlohmann::json::array({action("cancel-1", "FINISHED")})), kSent + seconds(10), kSent + seconds(10), kPolicy).outcome, Outcome::finished);
}

TEST(CancelTrackerTest, AnAcknowledgedCancelIsNeverSentAgain)
{
    auto tracker = tracker_for();
    const auto waiting = state("order-1", true, nlohmann::json::array({action("cancel-1", "WAITING")}));
    for (int t = 1; t < 60; ++t)
    {
        EXPECT_NE(tracker.assess(waiting, kSent + seconds(t), kSent + seconds(t), kPolicy).outcome, Outcome::resend) << t;
    }
}

TEST(CancelTrackerTest, ClearForgetsTheCancel)
{
    auto tracker = tracker_for();
    tracker.clear();
    EXPECT_FALSE(tracker.pending());
    EXPECT_EQ(tracker.assess(state("order-1", true), kSent + seconds(30), kSent + seconds(30), kPolicy).outcome, Outcome::none);
}

namespace {

// Write a config file whose vda5050 section ends with `extra` and return its path.
std::string write_config(const std::string &extra)
{
    const std::string path = ::testing::TempDir() + "cancel_config_test.yaml";
    std::ofstream(path) << "vda5050:\n  interface_name: test\n  mqtt:\n    host: localhost\n" << extra;
    return path;
}

bool rejected_naming(const std::string &extra, const std::string &key)
{
    try
    {
        vda5050_fleet_adapter_full_control::core::Config config(write_config(extra));
    }
    catch (const std::runtime_error &e)
    {
        return std::string(e.what()).find("vda5050." + key) != std::string::npos;
    }
    return false;
}

}  // namespace

TEST(CancelConfigTest, DefaultsApplyWhenTheKeysAreAbsent)
{
    const vda5050_fleet_adapter_full_control::core::Config config(write_config(""));
    EXPECT_EQ(config.cancel_policy().confirm_timeout, CancelPolicy{}.confirm_timeout);
    EXPECT_EQ(config.cancel_policy().attempts, CancelPolicy{}.attempts);
}

TEST(CancelConfigTest, BothKeysAreReadAndZeroTurnsTheTrackingOff)
{
    const vda5050_fleet_adapter_full_control::core::Config config(write_config("  cancel_confirm_timeout_s: 9\n  cancel_attempts: 2\n"));
    EXPECT_EQ(config.cancel_policy().confirm_timeout, seconds(9));
    EXPECT_EQ(config.cancel_policy().attempts, 2);
    const vda5050_fleet_adapter_full_control::core::Config off(write_config("  cancel_confirm_timeout_s: 0\n"));
    EXPECT_EQ(off.cancel_policy().confirm_timeout, seconds(0));
}

TEST(CancelConfigTest, ValuesOutOfRangeAreRejectedByName)
{
    EXPECT_TRUE(rejected_naming("  cancel_confirm_timeout_s: -1\n", "cancel_confirm_timeout_s"));
    EXPECT_TRUE(rejected_naming("  cancel_confirm_timeout_s: 121\n", "cancel_confirm_timeout_s"));
    EXPECT_TRUE(rejected_naming("  cancel_confirm_timeout_s: soon\n", "cancel_confirm_timeout_s"));
    EXPECT_TRUE(rejected_naming("  cancel_confirm_timeout_s: 2.5\n", "cancel_confirm_timeout_s"));
    EXPECT_TRUE(rejected_naming("  cancel_attempts: 0\n", "cancel_attempts"));
    EXPECT_TRUE(rejected_naming("  cancel_attempts: 11\n", "cancel_attempts"));
}
