#include "vda5050_client_adapter/adapter_state_machine.hpp"

namespace vda5050_adapter {

void AdapterStateMachine::mark_initialized()
{
  std::lock_guard<std::mutex> lock(mutex_);
  initialized_ = true;
  recompute_mode_locked();
}

void AdapterStateMachine::start_shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  shutting_down_ = true;
  recompute_mode_locked();
}

void AdapterStateMachine::on_mqtt_connection_changed(bool connected)
{
  std::lock_guard<std::mutex> lock(mutex_);
  mqtt_connected_ = connected;
  recompute_mode_locked();
}

void AdapterStateMachine::on_order_state_changed(bool order_active)
{
  std::lock_guard<std::mutex> lock(mutex_);
  order_active_ = order_active;
  recompute_mode_locked();
}

void AdapterStateMachine::on_action_blocking_changed(bool action_blocked)
{
  std::lock_guard<std::mutex> lock(mutex_);
  action_blocked_ = action_blocked;
  recompute_mode_locked();
}

void AdapterStateMachine::on_driver_driving_changed(bool driving)
{
  std::lock_guard<std::mutex> lock(mutex_);
  driver_driving_ = driving;
  recompute_mode_locked();
}

void AdapterStateMachine::on_driver_paused_changed(bool paused)
{
  std::lock_guard<std::mutex> lock(mutex_);
  driver_paused_ = paused;
  recompute_mode_locked();
}

void AdapterStateMachine::on_fatal_error_changed(bool fatal_error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  fatal_error_ = fatal_error;
  recompute_mode_locked();
}

void AdapterStateMachine::request_pause(const std::string& action_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  pending_pause_action_id_ = action_id;
  pending_resume_action_id_.clear();
  recompute_mode_locked();
}

void AdapterStateMachine::request_resume(const std::string& action_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  pending_resume_action_id_ = action_id;
  pending_pause_action_id_.clear();
  recompute_mode_locked();
}

void AdapterStateMachine::request_cancel(const std::string& action_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  pending_cancel_action_id_ = action_id;
  recompute_mode_locked();
}

std::vector<CompletedControlAction> AdapterStateMachine::consume_ready_control_actions()
{
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<CompletedControlAction> completed;

  if (!pending_pause_action_id_.empty() && driver_paused_) {
    completed.push_back(
      {ControlActionKind::START_PAUSE, pending_pause_action_id_, "Pause activated"});
    pending_pause_action_id_.clear();
  }

  if (!pending_resume_action_id_.empty() && !driver_paused_) {
    completed.push_back(
      {ControlActionKind::STOP_PAUSE, pending_resume_action_id_, "Pause deactivated"});
    pending_resume_action_id_.clear();
  }

  if (!pending_cancel_action_id_.empty() && !driver_driving_ && !order_active_) {
    completed.push_back(
      {ControlActionKind::CANCEL_ORDER, pending_cancel_action_id_, "Order cancelled"});
    pending_cancel_action_id_.clear();
  }

  recompute_mode_locked();
  return completed;
}

AdapterMode AdapterStateMachine::mode() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}

bool AdapterStateMachine::mqtt_connected() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return mqtt_connected_;
}

bool AdapterStateMachine::paused() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return driver_paused_;
}

bool AdapterStateMachine::reported_driving() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return driver_driving_ && !driver_paused_ && !action_blocked_ && !fatal_error_;
}

const char* AdapterStateMachine::to_string(AdapterMode mode)
{
  switch (mode) {
    case AdapterMode::INITIALIZING:
      return "INITIALIZING";
    case AdapterMode::CONNECTING:
      return "CONNECTING";
    case AdapterMode::IDLE:
      return "IDLE";
    case AdapterMode::ORDER_ACTIVE:
      return "ORDER_ACTIVE";
    case AdapterMode::ACTION_BLOCKED:
      return "ACTION_BLOCKED";
    case AdapterMode::PAUSE_PENDING:
      return "PAUSE_PENDING";
    case AdapterMode::RESUME_PENDING:
      return "RESUME_PENDING";
    case AdapterMode::PAUSED:
      return "PAUSED";
    case AdapterMode::CANCELLING:
      return "CANCELLING";
    case AdapterMode::FAULTED:
      return "FAULTED";
    case AdapterMode::SHUTTING_DOWN:
      return "SHUTTING_DOWN";
  }
  return "UNKNOWN";
}

void AdapterStateMachine::recompute_mode_locked()
{
  if (shutting_down_) {
    mode_ = AdapterMode::SHUTTING_DOWN;
    return;
  }

  if (!initialized_) {
    mode_ = AdapterMode::INITIALIZING;
    return;
  }

  if (fatal_error_) {
    mode_ = AdapterMode::FAULTED;
    return;
  }

  if (!mqtt_connected_) {
    mode_ = AdapterMode::CONNECTING;
    return;
  }

  if (!pending_cancel_action_id_.empty()) {
    mode_ = AdapterMode::CANCELLING;
    return;
  }

  if (!pending_pause_action_id_.empty()) {
    mode_ = AdapterMode::PAUSE_PENDING;
    return;
  }

  if (!pending_resume_action_id_.empty()) {
    mode_ = AdapterMode::RESUME_PENDING;
    return;
  }

  if (driver_paused_) {
    mode_ = AdapterMode::PAUSED;
    return;
  }

  if (action_blocked_) {
    mode_ = AdapterMode::ACTION_BLOCKED;
    return;
  }

  mode_ = order_active_ ? AdapterMode::ORDER_ACTIVE : AdapterMode::IDLE;
}

}  // namespace vda5050_adapter
