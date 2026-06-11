#include <gtest/gtest.h>

#include "tb3_vda5050_bridge/bridge_state_machine.hpp"

namespace {

using tb3_vda5050_bridge::BridgeMode;
using tb3_vda5050_bridge::BridgeStateMachine;

TEST(BridgeStateMachineTest, InitialStateIsIdle) {
  BridgeStateMachine machine;
  const auto status = machine.status();

  EXPECT_EQ(status.mode, BridgeMode::IDLE);
  EXPECT_FALSE(status.driving);
  EXPECT_FALSE(status.paused);
}

TEST(BridgeStateMachineTest, PauseResumeMaintainsDrivingPausedInvariant) {
  BridgeStateMachine machine;

  machine.on_order_started();
  machine.on_navigation_active();
  auto status = machine.status();
  EXPECT_EQ(status.mode, BridgeMode::NAVIGATING);
  EXPECT_TRUE(status.driving);
  EXPECT_FALSE(status.paused);

  machine.on_pause_requested();
  status = machine.status();
  EXPECT_EQ(status.mode, BridgeMode::PAUSED);
  EXPECT_FALSE(status.driving);
  EXPECT_TRUE(status.paused);

  machine.on_resume_requested();
  status = machine.status();
  EXPECT_EQ(status.mode, BridgeMode::DISPATCHING);
  EXPECT_FALSE(status.driving);
  EXPECT_FALSE(status.paused);
}

TEST(BridgeStateMachineTest, WaitingForReleaseAndFaultedAreNonDriving) {
  BridgeStateMachine machine;

  machine.on_waiting_for_release();
  auto status = machine.status();
  EXPECT_EQ(status.mode, BridgeMode::WAITING_FOR_RELEASE);
  EXPECT_FALSE(status.driving);
  EXPECT_FALSE(status.paused);

  machine.on_navigation_failed();
  status = machine.status();
  EXPECT_EQ(status.mode, BridgeMode::FAULTED);
  EXPECT_FALSE(status.driving);
  EXPECT_FALSE(status.paused);
}

TEST(BridgeStateMachineTest, CancelAndCompletionReturnToIdle) {
  BridgeStateMachine machine;

  machine.on_order_started();
  machine.on_cancel_requested();
  EXPECT_EQ(machine.status().mode, BridgeMode::IDLE);

  machine.on_order_started();
  machine.on_all_work_completed();
  EXPECT_EQ(machine.status().mode, BridgeMode::IDLE);
}

}  // namespace
