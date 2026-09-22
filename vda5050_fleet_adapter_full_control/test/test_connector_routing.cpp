#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::Transform;

namespace {

nlohmann::json state()
{
    using nlohmann::json;
    return {{"headerId", 1}, {"orderId", ""}, {"lastNodeId", "n0"}, {"driving", false},
            {"nodeStates", json::array()}, {"edgeStates", json::array()}, {"actionStates", json::array()},
            {"errors", json::array()}, {"operatingMode", "AUTOMATIC"},
            {"safetyState", {{"eStop", "NONE"}, {"fieldViolation", false}}},
            {"batteryState", {{"batteryCharge", 80.0}}},
            {"agvPosition", {{"x", 1.0}, {"y", 2.0}, {"theta", 0.0}, {"mapId", "map"}, {"positionInitialized", true}}}};
}

// A connector that is never started: messages are fed in directly.
class RoutingTest : public ::testing::Test
{
protected:
    RoutingTest() : connector(rclcpp::get_logger("test"), "tcp://localhost:1", "AMR") {}

    void feed(const std::string &topic, const nlohmann::json &payload)
    {
        connector.handle_message(topic, payload.dump());
    }

    bool online(const std::string &name) { return connector.is_online(name); }

    Connector connector;
};

}  // namespace

TEST_F(RoutingTest, AMessageReachesOnlyTheRobotWithThatIdentity)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    connector.add_robot("tb3_2", "ROBOTIS", "0002", Transform());
    feed("AMR/v2/ROBOTIS/0002/state", state());
    EXPECT_FALSE(online("tb3_1"));
    EXPECT_TRUE(online("tb3_2"));
}

TEST_F(RoutingTest, OnlineStatusFollowsTheConfiguredStateTimeout)
{
    vda5050_fleet_adapter_full_control::rmf::LinkPolicy policy;
    policy.state_timeout_s = 0.05;
    connector.set_link_policy(policy);
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    feed("AMR/v2/ROBOTIS/0001/state", state());
    EXPECT_TRUE(online("tb3_1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(online("tb3_1"));
}

TEST_F(RoutingTest, IdentitiesThatOverlapInATopicAreNotConfused)
{
    connector.add_robot("odd", "v2", "ACME", Transform());
    connector.add_robot("real", "ACME", "S1", Transform());
    feed("AMR/v2/ACME/S1/state", state());
    EXPECT_TRUE(online("real"));
    EXPECT_FALSE(online("odd"));
}

TEST_F(RoutingTest, TheLeafDecidesWhatAMessageMeans)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    connector.add_robot("tb3_2", "ROBOTIS", "0002", Transform());
    feed("AMR/v2/ROBOTIS/0001/state", state());
    feed("AMR/v2/ROBOTIS/0002/state", state());
    feed("AMR/v2/ROBOTIS/0002/connection", {{"connectionState", "CONNECTIONBROKEN"}});
    EXPECT_TRUE(online("tb3_1"));
    EXPECT_FALSE(online("tb3_2"));
}

TEST_F(RoutingTest, TopicsOfAnotherInterfaceAreIgnored)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    feed("OTHER/v2/ROBOTIS/0001/state", state());
    EXPECT_FALSE(online("tb3_1"));
}

TEST_F(RoutingTest, TopicsThatAreNotFiveLevelsOfTheProtocolVersionAreIgnored)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    for (const char *topic : {"AMR/v1/ROBOTIS/0001/state", "AMR/v2/ROBOTIS/0001/extra/state", "AMR/v2/ROBOTIS/state",
                              "AMR//ROBOTIS/0001/state", "AMR/v2//0001/state", "AMR/v2/ROBOTIS//state", "state", ""})
    {
        feed(topic, state());
        EXPECT_FALSE(online("tb3_1")) << topic;
    }
}

TEST_F(RoutingTest, AnUnregisteredIdentityIsDiscoveredNotRouted)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    feed("AMR/v2/ROBOTIS/0009/connection", {{"connectionState", "ONLINE"}});
    EXPECT_FALSE(online("tb3_1"));
    EXPECT_TRUE(connector.find_discovered("ROBOTIS", "0009").has_value());
}

TEST_F(RoutingTest, AnIdentityThatCannotBeATopicLevelIsRejected)
{
    const std::pair<const char *, const char *> bad[] = {{"", "0001"}, {"ROBOTIS", ""}, {"ROBO/TIS", "0001"},
                                                          {"ROBOTIS", "00/01"}, {"+", "0001"}, {"ROBOTIS", "#"}};
    for (const auto &[manufacturer, serial] : bad)
    {
        EXPECT_THROW(connector.add_robot("bad", manufacturer, serial, Transform()), std::invalid_argument)
            << manufacturer << "/" << serial;
    }
}

namespace {

// State with the given headerId and a timestamp that moves with it, in seconds after midnight.
nlohmann::json state_with_header(unsigned header)
{
    nlohmann::json message = state();
    message["headerId"] = header;
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "2026-01-01T00:00:%02u.000Z", header % 60);
    message["timestamp"] = stamp;
    return message;
}

}  // namespace

TEST_F(RoutingTest, MetricsCountEachKindOfMessage)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    feed("AMR/v2/ROBOTIS/0001/state", state());
    feed("AMR/v2/ROBOTIS/0001/state", state_with_header(2));
    feed("AMR/v2/ROBOTIS/0001/visualization", {{"agvPosition", {{"x", 1.0}, {"y", 2.0}, {"theta", 0.0}, {"mapId", "map"}, {"positionInitialized", true}}}});
    feed("AMR/v2/ROBOTIS/0001/connection", {{"connectionState", "ONLINE"}});
    feed("AMR/v2/ROBOTIS/0001/factsheet", nlohmann::json::object());
    feed("AMR/v2/OTHER/0009/state", state());
    connector.handle_message("AMR/v2/ROBOTIS/0001/state", "{not json");
    connector.handle_message("AMR/v2/ROBOTIS/state", "{}");

    const nlohmann::json report = connector.metrics();
    EXPECT_EQ(report["rx"]["state"], 2);
    EXPECT_EQ(report["rx"]["visualization"], 1);
    EXPECT_EQ(report["rx"]["connection"], 1);
    EXPECT_EQ(report["rx"]["factsheet"], 1);
    EXPECT_EQ(report["rx"]["unregistered"], 1);
    EXPECT_EQ(report["dropped"]["bad_payload"], 1);
    EXPECT_EQ(report["dropped"]["bad_topic"], 1);
    EXPECT_EQ(report["dropped"]["stale_state"], 0);
    EXPECT_EQ(report["dropped"]["oversize"], 0);
}

TEST_F(RoutingTest, MetricsCountStaleAndInvalidStates)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    feed("AMR/v2/ROBOTIS/0001/state", state_with_header(5));
    feed("AMR/v2/ROBOTIS/0001/state", state_with_header(4));
    nlohmann::json incomplete = state_with_header(6);
    incomplete.erase("driving");
    feed("AMR/v2/ROBOTIS/0001/state", incomplete);

    const nlohmann::json report = connector.metrics();
    EXPECT_EQ(report["dropped"]["stale_state"], 1);
    EXPECT_EQ(report["dropped"]["invalid_state"], 1);
}

TEST_F(RoutingTest, MetricsShowWhichRobotsHaveState)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    connector.add_robot("tb3_2", "ROBOTIS", "0002", Transform());
    feed("AMR/v2/ROBOTIS/0001/state", state());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const nlohmann::json report = connector.metrics();
    EXPECT_EQ(report["robots"]["registered"], 2);
    EXPECT_EQ(report["robots"]["online"], 1);
    EXPECT_EQ(report["robots"]["without_state"], 1);
    EXPECT_EQ(report["robots"]["oldest_state_robot"], "tb3_1");
    EXPECT_DOUBLE_EQ(report["robots"]["state_timeout_s"].get<double>(), vda5050_fleet_adapter_full_control::rmf::LinkPolicy{}.state_timeout_s);
    EXPECT_GE(report["robots"]["state_age_max_s"].get<double>(), 0.02);
}

TEST_F(RoutingTest, LatencyIsPerIntervalWhileCountsKeepGrowing)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    for (unsigned header = 1; header <= 20; ++header)
    {
        feed("AMR/v2/ROBOTIS/0001/state", state_with_header(header));
    }
    const nlohmann::json first = connector.metrics();
    EXPECT_EQ(first["latency_us"]["handle_state"]["count"], 20);
    EXPECT_EQ(first["latency_us"]["mutex_wait"]["count"], 20);
    EXPECT_GT(first["latency_us"]["handle_state"]["max"].get<double>(), 0.0);

    const nlohmann::json second = connector.metrics();
    EXPECT_EQ(second["latency_us"]["handle_state"]["count"], 0);
    EXPECT_EQ(second["rx"]["state"], 20);
}

TEST_F(RoutingTest, ARefusedPublishIsCounted)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    connector.request_state("tb3_1");
    const nlohmann::json report = connector.metrics();
    EXPECT_EQ(report["published"]["failed"], 1);
    EXPECT_EQ(report["published"]["ok"], 0);
    EXPECT_FALSE(report["mqtt"]["connected"].get<bool>());
}

TEST_F(RoutingTest, StateTransitIsMeasuredFromTheMessageTimestamp)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    nlohmann::json message = state();
    message["timestamp"] = "2020-01-01T00:00:00.000Z";
    feed("AMR/v2/ROBOTIS/0001/state", message);
    const nlohmann::json report = connector.metrics();
    EXPECT_EQ(report["state_transit_us"]["count"], 1);
    EXPECT_GT(report["state_transit_us"]["max"].get<double>(), 1e9);
}

TEST_F(RoutingTest, MetricsCanBeReadWhileMessagesArrive)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done)
        {
            (void)connector.metrics();
            (void)connector.is_online("tb3_1");
        }
    });
    for (unsigned header = 1; header <= 3000; ++header)
    {
        feed("AMR/v2/ROBOTIS/0001/state", state_with_header(header));
    }
    done = true;
    reader.join();
    EXPECT_EQ(connector.metrics()["rx"]["state"], 3000);
}
