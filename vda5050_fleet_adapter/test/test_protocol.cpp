#include <gtest/gtest.h>

#include "vda5050_fleet_adapter/vda5050_protocol.hpp"

namespace proto = vda5050_fleet_adapter::protocol;

TEST(Protocol, TopicFormat)
{
  EXPECT_EQ(proto::topic("TB3", "ROBOTIS", "0001", "order"),
            "TB3/v2/ROBOTIS/0001/order");
}

TEST(Protocol, NodeAndEdgeSequencing)
{
  const auto n0 = proto::make_node("base", 0, 1.0, 2.0, 0.5, "tb3_world");
  EXPECT_EQ(n0["nodeId"], "base");
  EXPECT_EQ(n0["sequenceId"], 0);
  EXPECT_TRUE(n0["released"]);
  EXPECT_DOUBLE_EQ(n0["nodePosition"]["x"], 1.0);
  EXPECT_EQ(n0["nodePosition"]["mapId"], "tb3_world");

  const auto e = proto::make_edge("e", 1, "base", "dest");
  EXPECT_EQ(e["sequenceId"], 1);
  EXPECT_EQ(e["startNodeId"], "base");
  EXPECT_EQ(e["endNodeId"], "dest");
  EXPECT_FALSE(e.contains("maxSpeed"));

  const auto es = proto::make_edge("e", 1, "a", "b", true, 0.4);
  EXPECT_DOUBLE_EQ(es["maxSpeed"], 0.4);
}

TEST(Protocol, OrderHasHeaderAndUniqueId)
{
  nlohmann::json nodes = nlohmann::json::array();
  nodes.push_back(proto::make_node("base", 0, 0, 0, 0, "m"));
  nodes.push_back(proto::make_node("dest", 2, 1, 1, 0, "m"));
  nlohmann::json edges = nlohmann::json::array();
  edges.push_back(proto::make_edge("e", 1, "base", "dest"));

  const auto o1 = proto::make_order(0, "ROBOTIS", "0001", nodes, edges);
  const auto o2 = proto::make_order(1, "ROBOTIS", "0001", nodes, edges);
  EXPECT_EQ(o1["version"], "2.1.0");
  EXPECT_EQ(o1["manufacturer"], "ROBOTIS");
  EXPECT_EQ(o1["orderUpdateId"], 0);
  EXPECT_EQ(o1["nodes"].size(), 2u);
  EXPECT_NE(o1["orderId"], o2["orderId"]);  // fresh UUID each call
}

TEST(Protocol, CancelOrderAction)
{
  const auto a = proto::cancel_order_action();
  EXPECT_EQ(a["actionType"], "cancelOrder");
  EXPECT_EQ(a["blockingType"], "HARD");
  EXPECT_FALSE(a["actionId"].get<std::string>().empty());
}

TEST(Protocol, ParseStatePositionAndBattery)
{
  const auto raw = nlohmann::json::parse(R"({
    "agvPosition": {"x": 12.8, "y": -14.7, "theta": 1.2, "mapId": "tb3_world",
                    "positionInitialized": true},
    "batteryState": {"batteryCharge": 80.0},
    "orderId": "ord-1", "lastNodeId": "dest", "driving": false,
    "nodeStates": [], "edgeStates": [], "actionStates": [], "errors": []
  })");
  proto::ParsedState s(raw);
  EXPECT_TRUE(s.has_position());
  EXPECT_DOUBLE_EQ(*s.x, 12.8);
  EXPECT_NEAR(*s.battery_soc, 0.8, 1e-9);
  EXPECT_EQ(s.last_node_id, "dest");
  EXPECT_FALSE(s.driving);
}

TEST(Protocol, OrderFinishedLogic)
{
  auto base = nlohmann::json::parse(R"({
    "agvPosition": {"x": 1, "y": 1, "theta": 0, "positionInitialized": true},
    "orderId": "ord-1", "driving": false,
    "nodeStates": [], "edgeStates": []
  })");
  EXPECT_TRUE(proto::ParsedState(base).order_finished("ord-1"));

  base["driving"] = true;
  EXPECT_FALSE(proto::ParsedState(base).order_finished("ord-1"));

  base["driving"] = false;
  base["orderId"] = "ord-2";  // AGV moved to a different order
  EXPECT_FALSE(proto::ParsedState(base).order_finished("ord-1"));

  base["nodeStates"] = nlohmann::json::array({{{"nodeId", "n"}}});
  base["orderId"] = "ord-1";
  EXPECT_FALSE(proto::ParsedState(base).order_finished("ord-1"));
}

TEST(Protocol, HasPositionRequiresInitialized)
{
  auto raw = nlohmann::json::parse(R"({
    "agvPosition": {"x": 1, "y": 2, "theta": 0, "positionInitialized": false}
  })");
  EXPECT_FALSE(proto::ParsedState(raw).has_position());
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
