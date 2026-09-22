#include <gtest/gtest.h>

#include <string>

#include "vda5050_fleet_adapter_full_control/core/metrics_report.hpp"

using vda5050_fleet_adapter_full_control::core::metrics_summary;
using json = nlohmann::json;

namespace {

json distribution(unsigned count, double p50, double p99, double max)
{
    return {{"count", count}, {"mean", p50}, {"p50", p50}, {"p99", p99}, {"max", max}};
}

// A report with the counts and latencies the summary reads.
json report(unsigned states, unsigned stale, unsigned overruns)
{
    return {{"interval_s", 60.0},
            {"robots", {{"registered", 9}, {"online", 8}, {"state_age_max_s", 1.5}, {"oldest_state_robot", "tb3_2"}}},
            {"rx", {{"state", states}, {"visualization", states * 2}, {"connection", 1}, {"factsheet", 0}, {"unregistered", states / 2}}},
            {"dropped", {{"bad_payload", 0}, {"bad_topic", 0}, {"unregistered", 0}, {"invalid_state", 0}, {"stale_state", stale}, {"oversize", 0}}},
            {"log_suppressed", stale},
            {"published", {{"ok", 10}, {"failed", 1}}},
            {"mqtt", {{"connections_lost", 2}}},
            {"latency_us", {{"handle_state", distribution(120, 52.0, 180.0, 900.0)}, {"mutex_wait", distribution(0, 0, 0, 0)}}},
            {"update_loop", {{"pass_us", distribution(600, 800.0, 2100.0, 12000.0)}, {"overruns", overruns}}}};
}

bool has(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(MetricsSummaryTest, TheFirstReportCountsFromZero)
{
    const std::string line = metrics_summary(report(120, 3, 1), json::object());
    EXPECT_TRUE(has(line, "[metrics] 60 s"));
    EXPECT_TRUE(has(line, "robots 8/9 online, state age max 1.50 s (tb3_2)"));
    EXPECT_TRUE(has(line, "rx state +120 viz +240 conn +1 fact +0, other robots +60"));
    EXPECT_TRUE(has(line, "dropped +3 (stale +3, oversize +0, log lines held back +3)"));
    EXPECT_TRUE(has(line, "overruns +1"));
    EXPECT_TRUE(has(line, "published +10 failed +1, mqtt lost +2"));
}

TEST(MetricsSummaryTest, LaterReportsShowTheGrowthSinceTheLastOne)
{
    const std::string line = metrics_summary(report(300, 5, 4), report(120, 3, 1));
    EXPECT_TRUE(has(line, "rx state +180 viz +360 conn +0"));
    EXPECT_TRUE(has(line, "dropped +2 (stale +2"));
    EXPECT_TRUE(has(line, "overruns +3"));
}

TEST(MetricsSummaryTest, LatencyIsShownInTheUnitThatReadsBest)
{
    const std::string line = metrics_summary(report(1, 0, 0), json::object());
    EXPECT_TRUE(has(line, "handle state n=120 p50 52 us p99 180 us max 900 us"));
    EXPECT_TRUE(has(line, "loop pass n=600 p50 800 us p99 2.1 ms max 12.0 ms"));
    json slow = report(1, 0, 0);
    slow["latency_us"]["handle_state"] = distribution(1, 2500000.0, 2500000.0, 2500000.0);
    EXPECT_TRUE(has(metrics_summary(slow, json::object()), "p99 2.50 s"));
}

TEST(MetricsSummaryTest, AnIdleDistributionSaysSo)
{
    const std::string line = metrics_summary(report(1, 0, 0), json::object());
    EXPECT_TRUE(has(line, "mutex wait idle"));
}

TEST(MetricsSummaryTest, MissingPartsDoNotBreakTheLine)
{
    const std::string line = metrics_summary(json::object(), json::object());
    EXPECT_TRUE(has(line, "robots 0/0 online"));
    EXPECT_TRUE(has(line, "handle state idle"));
    EXPECT_FALSE(has(line, "()"));
}

TEST(MetricsSummaryTest, ACounterThatWentBackwardsCountsFromZero)
{
    const std::string line = metrics_summary(report(10, 0, 0), report(500, 0, 0));
    EXPECT_TRUE(has(line, "rx state +10"));
}
