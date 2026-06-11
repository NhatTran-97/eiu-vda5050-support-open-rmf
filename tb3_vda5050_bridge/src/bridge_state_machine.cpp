#include "tb3_vda5050_bridge/bridge_state_machine.hpp"

namespace tb3_vda5050_bridge {

namespace {

BridgeStatus make_status(BridgeMode mode, bool driving, bool paused) {
  BridgeStatus status;
  status.mode = mode;
  status.driving = driving;
  status.paused = paused;
  return status;
}

}  // namespace

void BridgeStateMachine::on_order_started() {
  mode_ = BridgeMode::DISPATCHING;
}

void BridgeStateMachine::on_dispatching() {
  mode_ = BridgeMode::DISPATCHING;
}

void BridgeStateMachine::on_navigation_active() {
  mode_ = BridgeMode::NAVIGATING;
}

void BridgeStateMachine::on_waiting_for_release() {
  mode_ = BridgeMode::WAITING_FOR_RELEASE;
}

void BridgeStateMachine::on_pause_requested() {
  mode_ = BridgeMode::PAUSED;
}

void BridgeStateMachine::on_resume_requested() {
  mode_ = BridgeMode::DISPATCHING;
}

void BridgeStateMachine::on_cancel_requested() {
  mode_ = BridgeMode::IDLE;
}

void BridgeStateMachine::on_navigation_failed() {
  mode_ = BridgeMode::FAULTED;
}

void BridgeStateMachine::on_all_work_completed() {
  mode_ = BridgeMode::IDLE;
}

BridgeStatus BridgeStateMachine::status() const {
  return status_from_mode(mode_);
}

BridgeStatus BridgeStateMachine::status_from_mode(BridgeMode mode) {
  switch (mode) {
    case BridgeMode::NAVIGATING:
      return make_status(mode, true, false);
    case BridgeMode::PAUSED:
      return make_status(mode, false, true);
    case BridgeMode::DISPATCHING:
    case BridgeMode::WAITING_FOR_RELEASE:
    case BridgeMode::FAULTED:
    case BridgeMode::IDLE:
    default:
      return make_status(mode, false, false);
  }
}

std::string to_string(BridgeMode mode) 
{
  switch (mode) {
    case BridgeMode::DISPATCHING:
      return "DISPATCHING";
    case BridgeMode::NAVIGATING:
      return "NAVIGATING";
    case BridgeMode::WAITING_FOR_RELEASE:
      return "WAITING_FOR_RELEASE";
    case BridgeMode::PAUSED:
      return "PAUSED";
    case BridgeMode::FAULTED:
      return "FAULTED";
    case BridgeMode::IDLE:
    default:
      return "IDLE";
  }
}

}  // namespace tb3_vda5050_bridge
