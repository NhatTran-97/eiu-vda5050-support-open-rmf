#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace vda5050_adapter {

enum class AdapterMode {
  INITIALIZING,
  CONNECTING,
  IDLE,
  ORDER_ACTIVE,
  ACTION_BLOCKED,
  PAUSE_PENDING,
  RESUME_PENDING,
  PAUSED,
  CANCELLING,
  FAULTED,
  SHUTTING_DOWN
};

enum class ControlActionKind {
  START_PAUSE,
  STOP_PAUSE,
  CANCEL_ORDER
};

struct CompletedControlAction {
  ControlActionKind kind{ControlActionKind::START_PAUSE};
  std::string       action_id;
  std::string       result_description;
};

class AdapterStateMachine {
public:
  AdapterStateMachine() = default;
  ~AdapterStateMachine() = default;

  void mark_initialized();
  void start_shutdown();

  void on_mqtt_connection_changed(bool connected);
  void on_order_state_changed(bool order_active);
  void on_action_blocking_changed(bool action_blocked);
  void on_driver_driving_changed(bool driving);
  void on_driver_paused_changed(bool paused);
  void on_fatal_error_changed(bool fatal_error);

  void request_pause(const std::string& action_id);
  void request_resume(const std::string& action_id);
  void request_cancel(const std::string& action_id);

  // Clears and returns a still-pending cancelOrder action id (empty if none).
  // Used when a new order supersedes a cancelOrder that never reached the
  // order-inactive + stopped state (e.g. cancel immediately followed by a
  // replacement order while the robot is still driving).
  std::string take_pending_cancel();

  std::vector<CompletedControlAction> consume_ready_control_actions();

  AdapterMode mode() const;
  bool mqtt_connected() const;
  bool paused() const;
  bool reported_driving() const;

  static const char* to_string(AdapterMode mode);

private:
  void recompute_mode_locked();

  mutable std::mutex mutex_;

  AdapterMode mode_{AdapterMode::INITIALIZING};
  bool initialized_{false};
  bool shutting_down_{false};
  bool mqtt_connected_{false};
  bool order_active_{false};
  bool action_blocked_{false};
  bool driver_driving_{false};
  bool driver_paused_{false};
  bool fatal_error_{false};

  std::string pending_pause_action_id_;
  std::string pending_resume_action_id_;
  std::string pending_cancel_action_id_;
};

}  // namespace vda5050_adapter
