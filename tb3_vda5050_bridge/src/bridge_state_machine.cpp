#include "tb3_vda5050_bridge/bridge_state_machine.hpp"

namespace tb3_vda5050_bridge {

namespace {

// Create BridgeStatus with mode (mode), driving (driving), paused (paused).
BridgeStatus make_status(BridgeMode mode, bool driving, bool paused) {
  BridgeStatus status;
  status.mode = mode;
  status.driving = driving;
  status.paused = paused;
  return status;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// State transitions
// ─────────────────────────────────────────────────────────────────────────────

// Transition to DISPATCHING when order starts.
void BridgeStateMachine::on_order_started() {
  mode_ = BridgeMode::DISPATCHING;
}

// Transition to DISPATCHING when planning/retrying dispatch.
void BridgeStateMachine::on_dispatching() {
  mode_ = BridgeMode::DISPATCHING;
}

// Transition to NAVIGATING when Nav2 goal is active.
void BridgeStateMachine::on_navigation_active() {
  mode_ = BridgeMode::NAVIGATING;
}

// Transition to WAITING_FOR_RELEASE when order has unreleased nodes.
void BridgeStateMachine::on_waiting_for_release() {
  mode_ = BridgeMode::WAITING_FOR_RELEASE;
}

// Transition to PAUSED on pause request.
void BridgeStateMachine::on_pause_requested() {
  mode_ = BridgeMode::PAUSED;
}

// Transition to DISPATCHING on resume from pause.
void BridgeStateMachine::on_resume_requested() {
  mode_ = BridgeMode::DISPATCHING;
}

// Transition to IDLE on cancel.
void BridgeStateMachine::on_cancel_requested() {
  mode_ = BridgeMode::IDLE;
}

// Transition to FAULTED when navigation fails.
void BridgeStateMachine::on_navigation_failed() {
  mode_ = BridgeMode::FAULTED;
}

// Transition to IDLE when all work is complete.
void BridgeStateMachine::on_all_work_completed() {
  mode_ = BridgeMode::IDLE;
}

// ─────────────────────────────────────────────────────────────────────────────
// State queries
// ─────────────────────────────────────────────────────────────────────────────

// Return BridgeStatus derived from current mode.
BridgeStatus BridgeStateMachine::status() const {
  return status_from_mode(mode_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// Convert mode (mode) to BridgeStatus: set driving/paused flags based on mode.
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

// Convert BridgeMode (mode) to human-readable string for logging/debugging.
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
