#include <gtest/gtest.h>

#include "vda5050_fleet_adapter/robot_state_machine.hpp"

using vda5050_fleet_adapter::RobotStateMachine;

// CommandExecution needs a live RMF handle; only the destination -> nodeId mapping is tested.
TEST(StateMachine, DeriveNodeIdPrefersName)
{
  EXPECT_EQ(RobotStateMachine::derive_node_id("wp6", 3, 1.0, 2.0), "wp6");
}

TEST(StateMachine, DeriveNodeIdFallsBackToGraphIndex)
{
  EXPECT_EQ(RobotStateMachine::derive_node_id("", 3, 1.0, 2.0), "wp_3");
}

TEST(StateMachine, DeriveNodeIdFallsBackToCoordinates)
{
  EXPECT_EQ(RobotStateMachine::derive_node_id("", std::nullopt, 1.234, -5.678),
            "1.23_-5.68");
}

int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
