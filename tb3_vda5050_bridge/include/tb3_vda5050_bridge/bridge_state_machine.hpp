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

class BridgeStateMachine {
public:
  BridgeStateMachine() = default;

  void on_order_started();
  void on_dispatching();
  void on_navigation_active();
  void on_waiting_for_release();
  void on_pause_requested();
  void on_resume_requested();
  void on_cancel_requested();
  void on_navigation_failed();
  void on_all_work_completed();

  BridgeStatus status() const;
  BridgeMode mode() const { return mode_; }
  bool is_paused() const { return mode_ == BridgeMode::PAUSED; }

private:
  static BridgeStatus status_from_mode(BridgeMode mode);

  BridgeMode mode_{BridgeMode::IDLE};
};

std::string to_string(BridgeMode mode);

}  // namespace tb3_vda5050_bridge
