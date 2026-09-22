#pragma once

#include <string>

namespace tb3_vda5050_bridge {

enum class BridgeMode {
  IDLE,
  DISPATCHING,
  NAVIGATING,
  WAITING_FOR_RELEASE,
  PAUSED,
  FAULTED
};

struct BridgeStatus {
  BridgeMode mode{BridgeMode::IDLE};
  bool driving{false};
  bool paused{false};
};

// Derives the driving and paused flags from mode transitions.
class BridgeStateMachine {
public:
  BridgeStateMachine() = default;

  // New order received: DISPATCHING.
  void on_order_started();
  // Planning or retrying dispatch: DISPATCHING.
  void on_dispatching();
  // Nav2 accepted a goal: NAVIGATING.
  void on_navigation_active();
  // Next node not released yet: WAITING_FOR_RELEASE.
  void on_waiting_for_release();
  // Pause requested: PAUSED.
  void on_pause_requested();
  // Resume requested: DISPATCHING.
  void on_resume_requested();
  // Cancel or order end: IDLE.
  void on_cancel_requested();
  // Navigation failed for good: FAULTED.
  void on_navigation_failed();
  // All route work done: IDLE.
  void on_all_work_completed();

  // Mode with the driving and paused flags derived from it.
  BridgeStatus status() const;
  BridgeMode mode() const { return mode_; }
  bool is_paused() const { return mode_ == BridgeMode::PAUSED; }

private:
  // Driving and paused flags of a mode.
  static BridgeStatus status_from_mode(BridgeMode mode);

  BridgeMode mode_{BridgeMode::IDLE};
};

// Mode name for logs.
std::string to_string(BridgeMode mode);

}  // namespace tb3_vda5050_bridge
