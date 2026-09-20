#include <gtest/gtest.h>

#include <mqtt/message.h>
#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter/vda5050_connector.hpp"

using vda5050_fleet_adapter::Transform;
using vda5050_fleet_adapter::Vda5050Connector;

namespace {

constexpr const char* kBase = "EFCT/v2/TEST/0001/";

// A connector that is never started: incoming messages are fed in directly.
class ConnectorTest : public ::testing::Test
{
protected:
  ConnectorTest() : connector(rclcpp::get_logger("test"), "tcp://localhost:1", "EFCT")
  {
    connector.add_robot("r", "TEST", "0001", Transform());
  }

  void feed(const char* leaf, const nlohmann::json& payload)
  {
    connector.message_arrived(mqtt::make_message(std::string(kBase) + leaf, payload.dump()));
  }

  void feed_state(const nlohmann::json& extra = nlohmann::json::object())
  {
    nlohmann::json state = {
      {"agvPosition", {{"x", 1.0}, {"y", 2.0}, {"theta", 0.0}, {"mapId", "m"},
                       {"positionInitialized", true}}},
      {"orderId", ""}, {"lastNodeId", "start"}, {"driving", false},
    };
    state.update(extra);
    feed("state", state);
  }

  Vda5050Connector connector;
};

}  // namespace

TEST_F(ConnectorTest, FactsheetGatesCustomActionsButNotCoreActions)
{
  feed("factsheet", {{"protocolFeatures", {{"agvActions", nlohmann::json::array(
    {{{"actionType", "pick"}, {"blockingTypes", {"SOFT"}}}})}}}});

  EXPECT_FALSE(connector.execute_instant_action("r", "pick").empty());
  EXPECT_TRUE(connector.execute_instant_action("r", "drop").empty());
  EXPECT_FALSE(connector.execute_instant_action("r", "cancelOrder").empty());

  connector.set_strict_validation(false);
  EXPECT_FALSE(connector.execute_instant_action("r", "drop").empty());
}

TEST_F(ConnectorTest, WithoutFactsheetEveryActionIsSent)
{
  EXPECT_FALSE(connector.execute_instant_action("r", "anything").empty());
}

TEST_F(ConnectorTest, ReadinessFollowsConnectionAndState)
{
  EXPECT_FALSE(connector.readiness("r").ready);

  feed("connection", {{"connectionState", "ONLINE"}});
  feed_state();
  EXPECT_TRUE(connector.readiness("r").ready);

  feed_state({{"safetyState", {{"eStop", "AUTOACK"}, {"fieldViolation", false}}}});
  EXPECT_FALSE(connector.readiness("r").ready);

  feed_state({{"paused", true}});
  EXPECT_FALSE(connector.readiness("r").ready);
  EXPECT_TRUE(connector.readiness("r", true).ready);

  feed("connection", {{"connectionState", "CONNECTIONBROKEN"}});
  EXPECT_EQ(connector.readiness("r").reason, "offline");
}

TEST_F(ConnectorTest, UnknownRobotIsNotReady)
{
  EXPECT_EQ(connector.readiness("nobody").reason, "unknown robot");
  EXPECT_FALSE(connector.set_speed_limit("nobody", 0.2));
  EXPECT_TRUE(connector.set_speed_limit("r", 0.2));
}

TEST_F(ConnectorTest, ReportsKnownMapAndActionResult)
{
  EXPECT_FALSE(connector.get_known_map("r").has_value());

  feed_state({{"actionStates", nlohmann::json::array(
    {{{"actionId", "a1"}, {"actionStatus", "FAILED"}, {"resultDescription", "busy"}}})}});
  EXPECT_EQ(connector.get_known_map("r").value_or(""), "m");
  const auto result = connector.get_action_result("r", "a1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->first, "FAILED");
  EXPECT_EQ(result->second, "busy");
  EXPECT_FALSE(connector.get_action_result("r", "other").has_value());
}

TEST_F(ConnectorTest, ActiveOrderIsMatchedByDestinationAndOrderId)
{
  EXPECT_FALSE(connector.has_active_order_to("r", "B"));

  connector.navigate("r", "B", 5.0, 6.0, 0.0, "m");
  const std::string order_id = connector.current_order_id("r");
  ASSERT_FALSE(order_id.empty());

  feed_state({{"orderId", order_id}, {"driving", true}});
  EXPECT_TRUE(connector.has_active_order_to("r", "B"));
  EXPECT_FALSE(connector.has_active_order_to("r", "C"));

  feed_state({{"orderId", "another"}});
  EXPECT_FALSE(connector.has_active_order_to("r", "B"));

  feed_state({{"orderId", order_id}, {"lastNodeId", "B"}, {"driving", false}});
  EXPECT_FALSE(connector.has_active_order_to("r", "B"));
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
