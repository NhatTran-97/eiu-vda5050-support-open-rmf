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

/**
 * @brief Adapter-wide state machine: connectivity, order activity, control action confirmations.
 *
 * Manages top-level adapter mode transitions (INITIALIZING → CONNECTING → IDLE ↔ ORDER_ACTIVE).
 * Handles fatal errors, MQTT connectivity state, driving/paused flags from robot driver,
 * and control action (pause/resume/cancel) confirmation logic.
 *
 * Intentionally narrow: owns only mode, connectivity, and control confirmation. Does not own
 * order state (OrderManager) or per-action lifecycle (ActionManager) — those are delegated.
 *
 * Key responsibilities:
 *  - Manage 11-mode state machine (INITIALIZING, CONNECTING, IDLE, ORDER_ACTIVE, ACTION_BLOCKED, etc.)
 *  - Track MQTT connectivity and fatal error state
 *  - Confirm control actions (pause/resume/cancel) when driver confirms
 *  - Gate message processing and state publication based on mode
 */
class AdapterStateMachine {
public:
  AdapterStateMachine() = default;
  ~AdapterStateMachine() = default;

  // ── Initialization and shutdown ────────────────────────────────────────────

  /**
   * @brief Mark initialization complete; unblock transitions from INITIALIZING.
   */
  void mark_initialized();

  /**
   * @brief Signal adapter shutdown; begin transition toward shutdown/fault mode.
   */
  void start_shutdown();

  // ── State change notifications ─────────────────────────────────────────────

  // Update MQTT connection state (connected): recompute mode if changed.
  void on_mqtt_connection_changed(bool connected);
  // Update order active state (order_active): recompute mode if changed.
  void on_order_state_changed(bool order_active);
  // Update action blocking state (action_blocked): recompute mode if changed.
  void on_action_blocking_changed(bool action_blocked);
  // Update driver driving state (driving): recompute mode if changed.
  void on_driver_driving_changed(bool driving);
  // Update driver paused state (paused): recompute mode if changed.
  void on_driver_paused_changed(bool paused);
  // Update fatal error state (fatal_error): transition to FAULTED if set.
  void on_fatal_error_changed(bool fatal_error);

  // ── Control action requests ────────────────────────────────────────────────

  // Request pause with action_id (action_id); return old pause request id (if replaced) or empty.
  std::string request_pause(const std::string& action_id);
  // Request resume with action_id (action_id); return old resume request id (if replaced) or empty.
  std::string request_resume(const std::string& action_id);
  // Request cancel with action_id (action_id); return old cancel request id (if replaced) or empty.
  std::string request_cancel(const std::string& action_id);

  // ── Control action processing ──────────────────────────────────────────────

  // Clear and return pending cancelOrder action id (empty if none).
  std::string take_pending_cancel();

  // Consume and return control actions ready to execute; clears internal queue.
  std::vector<CompletedControlAction> consume_ready_control_actions();

  // ── State queries ──────────────────────────────────────────────────────────

  // Return current adapter mode.
  AdapterMode mode() const;
  // Return true if MQTT is currently connected.
  bool mqtt_connected() const;
  // Return true if adapter is in PAUSED mode.
  bool paused() const;
  // Return true if last reported driver state was driving.
  bool reported_driving() const;

  // Convert AdapterMode (mode) to human-readable string.
  static const char* to_string(AdapterMode mode);

private:
  // ── Internal helpers ───────────────────────────────────────────────────────

  // Recompute mode from current flags; update mode_ if changed.
  void recompute_mode_locked();

  // ── State ──────────────────────────────────────────────────────────────────

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
