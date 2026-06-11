#include <gtest/gtest.h>

#include "vda5050_client_adapter/adapter_state_machine.hpp"

namespace {

using vda5050_adapter::AdapterMode;
using vda5050_adapter::AdapterStateMachine;

TEST(AdapterStateMachineTest, TracksTopLevelModesAcrossConnectionOrderAndFault)
{
  AdapterStateMachine sm;

  EXPECT_EQ(sm.mode(), AdapterMode::INITIALIZING);

  sm.mark_initialized();
  EXPECT_EQ(sm.mode(), AdapterMode::CONNECTING);

  sm.on_mqtt_connection_changed(true);
  EXPECT_EQ(sm.mode(), AdapterMode::IDLE);

  sm.on_order_state_changed(true);
  EXPECT_EQ(sm.mode(), AdapterMode::ORDER_ACTIVE);

  sm.on_action_blocking_changed(true);
  EXPECT_EQ(sm.mode(), AdapterMode::ACTION_BLOCKED);
  EXPECT_FALSE(sm.reported_driving());

  sm.on_action_blocking_changed(false);
  EXPECT_EQ(sm.mode(), AdapterMode::ORDER_ACTIVE);

  sm.on_fatal_error_changed(true);
  EXPECT_EQ(sm.mode(), AdapterMode::FAULTED);
  EXPECT_FALSE(sm.reported_driving());

  sm.on_fatal_error_changed(false);
  EXPECT_EQ(sm.mode(), AdapterMode::ORDER_ACTIVE);

  sm.start_shutdown();
  EXPECT_EQ(sm.mode(), AdapterMode::SHUTTING_DOWN);
}

TEST(AdapterStateMachineTest, CompletesPauseAndResumeControlActionsOnDriverFeedback)
{
  AdapterStateMachine sm;
  sm.mark_initialized();
  sm.on_mqtt_connection_changed(true);
  sm.on_order_state_changed(true);
  sm.on_driver_driving_changed(true);

  sm.request_pause("pause-1");
  EXPECT_EQ(sm.mode(), AdapterMode::PAUSE_PENDING);
  EXPECT_TRUE(sm.reported_driving());

  sm.on_driver_paused_changed(true);
  auto completed = sm.consume_ready_control_actions();
  ASSERT_EQ(completed.size(), 1u);
  EXPECT_EQ(completed.front().action_id, "pause-1");
  EXPECT_EQ(sm.mode(), AdapterMode::PAUSED);
  EXPECT_TRUE(sm.paused());
  EXPECT_FALSE(sm.reported_driving());

  sm.request_resume("resume-1");
  EXPECT_EQ(sm.mode(), AdapterMode::RESUME_PENDING);

  completed = sm.consume_ready_control_actions();
  EXPECT_TRUE(completed.empty());
  EXPECT_EQ(sm.mode(), AdapterMode::RESUME_PENDING);

  sm.on_driver_paused_changed(false);
  completed = sm.consume_ready_control_actions();
  ASSERT_EQ(completed.size(), 1u);
  EXPECT_EQ(completed.front().action_id, "resume-1");
  EXPECT_EQ(sm.mode(), AdapterMode::ORDER_ACTIVE);
  EXPECT_FALSE(sm.paused());
  EXPECT_TRUE(sm.reported_driving());
}

TEST(AdapterStateMachineTest, KeepsCancellingUntilOrderIsInactiveAndRobotStops)
{
  AdapterStateMachine sm;
  sm.mark_initialized();
  sm.on_mqtt_connection_changed(true);
  sm.on_order_state_changed(true);
  sm.on_driver_driving_changed(true);

  sm.request_cancel("cancel-1");
  EXPECT_EQ(sm.mode(), AdapterMode::CANCELLING);

  sm.on_order_state_changed(false);
  auto completed = sm.consume_ready_control_actions();
  EXPECT_TRUE(completed.empty());
  EXPECT_EQ(sm.mode(), AdapterMode::CANCELLING);

  sm.on_driver_driving_changed(false);
  completed = sm.consume_ready_control_actions();
  ASSERT_EQ(completed.size(), 1u);
  EXPECT_EQ(completed.front().action_id, "cancel-1");
  EXPECT_EQ(sm.mode(), AdapterMode::IDLE);
  EXPECT_FALSE(sm.reported_driving());
}

}  // namespace
