#include <gtest/gtest.h>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::Transform;

namespace {

// A connector that is never started: messages are fed in directly.
class DiscoveryTest : public ::testing::Test
{
protected:
    DiscoveryTest() : connector(rclcpp::get_logger("test"), "tcp://localhost:1", "AMR") {}

    void feed(const std::string &topic, const nlohmann::json &payload)
    {
        connector.handle_message(topic, payload.dump());
    }

    void connection(const std::string &maker, const std::string &serial, const std::string &state)
    {
        feed("AMR/v2/" + maker + "/" + serial + "/connection", {{"connectionState", state}});
    }

    Connector connector;
};

}  // namespace

TEST_F(DiscoveryTest, UnknownOnlineRobotIsDiscovered)
{
    EXPECT_TRUE(connector.discovered().empty());
    connection("ROBOTIS", "0003", "ONLINE");

    const auto all = connector.discovered();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].manufacturer, "ROBOTIS");
    EXPECT_EQ(all[0].serial, "0003");
    EXPECT_TRUE(all[0].connection_seen);
    EXPECT_TRUE(all[0].online);
    EXPECT_TRUE(connector.find_discovered("ROBOTIS", "0003").has_value());
    EXPECT_FALSE(connector.find_discovered("ROBOTIS", "9999").has_value());
}

TEST_F(DiscoveryTest, ConnectionStateFollowsTheLatestMessage)
{
    connection("ROBOTIS", "0003", "ONLINE");
    connection("ROBOTIS", "0003", "CONNECTIONBROKEN");
    EXPECT_FALSE(connector.find_discovered("ROBOTIS", "0003")->online);
    EXPECT_TRUE(connector.find_discovered("ROBOTIS", "0003")->connection_seen);
}

TEST_F(DiscoveryTest, RegisteredRobotsAreNotDiscovered)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    connection("ROBOTIS", "0001", "ONLINE");
    EXPECT_TRUE(connector.discovered().empty());
}

TEST_F(DiscoveryTest, RegisteringADiscoveredRobotRemovesItFromTheList)
{
    connection("ROBOTIS", "0003", "ONLINE");
    ASSERT_EQ(connector.discovered().size(), 1u);
    connector.add_robot("tb3_3", "ROBOTIS", "0003", Transform());
    EXPECT_TRUE(connector.discovered().empty());
}

TEST_F(DiscoveryTest, OtherInterfacesAndMalformedTopicsAreIgnored)
{
    feed("uagv/v2/KIT/0001/connection", {{"connectionState", "ONLINE"}});
    feed("AMR/v2/ROBOTIS/connection", {{"connectionState", "ONLINE"}});
    feed("AMR/v2/ROBOTIS/0003/extra/connection", {{"connectionState", "ONLINE"}});
    feed("AMR/v1/ROBOTIS/0003/connection", {{"connectionState", "ONLINE"}});
    feed("AMR/v2/ROBOTIS/0003/order", {{"orderId", "x"}});
    feed("AMR/v2//0003/connection", {{"connectionState", "ONLINE"}});
    connector.handle_message("AMR/v2/ROBOTIS/0003/connection", "not json");
    connector.handle_message("AMR/v2/ROBOTIS/0003/connection", "[1, 2]");
    EXPECT_TRUE(connector.discovered().empty());
}

TEST_F(DiscoveryTest, FactsheetAndPoseAreKeptForACandidate)
{
    connection("ROBOTIS", "0003", "ONLINE");
    feed("AMR/v2/ROBOTIS/0003/factsheet",
         {{"typeSpecification", {{"seriesName", "TurtleBot3 Burger"}}}, {"physicalParameters", {{"speedMax", 0.22}}}});
    feed("AMR/v2/ROBOTIS/0003/state",
         {{"agvPosition", {{"x", 1.5}, {"y", -2.0}, {"theta", 0.3}, {"mapId", "tb3_world"}, {"positionInitialized", true}}}});

    const auto robot = connector.find_discovered("ROBOTIS", "0003");
    ASSERT_TRUE(robot.has_value());
    ASSERT_TRUE(robot->factsheet.has_value());
    EXPECT_EQ(robot->factsheet->series_name, "TurtleBot3 Burger");
    EXPECT_TRUE(robot->has_state);
    EXPECT_TRUE(robot->pose_initialized);
    EXPECT_DOUBLE_EQ(robot->x, 1.5);
    EXPECT_DOUBLE_EQ(robot->y, -2.0);
    EXPECT_EQ(robot->map_id, "tb3_world");
}

TEST_F(DiscoveryTest, EmptyFactsheetAndStateWithoutPoseAreNotStored)
{
    connection("ROBOTIS", "0003", "ONLINE");
    feed("AMR/v2/ROBOTIS/0003/factsheet", nlohmann::json::object());
    feed("AMR/v2/ROBOTIS/0003/state", {{"orderId", ""}});
    const auto robot = connector.find_discovered("ROBOTIS", "0003");
    EXPECT_FALSE(robot->factsheet.has_value());
    EXPECT_FALSE(robot->has_state);
}

TEST_F(DiscoveryTest, WatchSubscribesOnlyNewOnlineRobotsOnce)
{
    connection("ROBOTIS", "0003", "ONLINE");
    connection("ROBOTIS", "0004", "CONNECTIONBROKEN");
    connector.watch_discovered();
    EXPECT_TRUE(connector.find_discovered("ROBOTIS", "0003")->watched);
    EXPECT_FALSE(connector.find_discovered("ROBOTIS", "0004")->watched);

    connection("ROBOTIS", "0004", "ONLINE");
    connector.watch_discovered();
    EXPECT_TRUE(connector.find_discovered("ROBOTIS", "0004")->watched);
}

TEST_F(DiscoveryTest, TheListIsBoundedAgainstAFloodOfIdentities)
{
    for (int i = 0; i < 400; ++i)
    {
        connection("M", std::to_string(i), "ONLINE");
    }
    EXPECT_EQ(connector.discovered().size(), 256u);
}

TEST_F(DiscoveryTest, OfflineRobotsMakeRoomForOnlineOnes)
{
    for (int i = 0; i < 300; ++i)
    {
        connection("OLD", std::to_string(i), "CONNECTIONBROKEN");
    }
    EXPECT_EQ(connector.discovered().size(), 256u);

    connection("ROBOTIS", "0003", "ONLINE");
    ASSERT_TRUE(connector.find_discovered("ROBOTIS", "0003").has_value());
    EXPECT_TRUE(connector.find_discovered("ROBOTIS", "0003")->online);
    EXPECT_EQ(connector.discovered().size(), 256u);
}

TEST_F(DiscoveryTest, RegisteredFactsheetsAreCollectedForTypeComparison)
{
    connector.add_robot("tb3_1", "ROBOTIS", "0001", Transform());
    EXPECT_TRUE(connector.registered_factsheets().empty());
    feed("AMR/v2/ROBOTIS/0001/factsheet", {{"typeSpecification", {{"seriesName", "TurtleBot3 Burger"}}}});
    ASSERT_EQ(connector.registered_factsheets().size(), 1u);
    EXPECT_EQ(connector.registered_factsheets()[0].series_name, "TurtleBot3 Burger");
}
