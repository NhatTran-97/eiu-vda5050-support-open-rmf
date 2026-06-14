#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "vda5050_client_adapter/vda5050_types.hpp"

namespace vda5050_adapter {

/**
 * @brief Manages VDA5050 action lifecycle for both order actions and
 *        instantActions.
 *
 * Lifecycle per action:  WAITING → INITIALIZING → RUNNING → FINISHED | FAILED
 *                                                          ↕  (PAUSED)
 *
 * Blocking types:
 *   NONE  — driving continues, parallel actions allowed
 *   SOFT  — driving stops until FINISHED/FAILED, parallel actions OK
 *   HARD  — no other action may run in parallel
 *
 * Thread-safe: all public methods may be called from any thread.
 */
class ActionManager {
public:
  // ─── Callbacks ────────────────────────────────────────────────────────────

  /// Start executing an action (robot driver).
  using ActionExecuteCallback =
    std::function<void(const vda5050::Action&)>;

  /// Pause a running action.
  using ActionPauseCallback =
    std::function<void(const std::string& action_id)>;

  /// Resume a paused action.
  using ActionResumeCallback =
    std::function<void(const std::string& action_id)>;

  /// Cancel an action.
  using ActionCancelCallback =
    std::function<void(const std::string& action_id)>;

  ActionManager();
  ~ActionManager() = default;

  // ─── Action ingestion ─────────────────────────────────────────────────────

  /// Queue node actions; dispatched when the node is reached.
  void enqueue_node_actions(const vda5050::Node& node);

  /// Queue edge actions; dispatched when the edge is entered.
  void enqueue_edge_actions(const vda5050::Edge& edge);

  /// Execute instantActions immediately.
  void process_instant_actions(const vda5050::InstantActions& instant_actions);

  /// Sync order actions against the current base + horizon graph.
  void sync_order_actions(const std::vector<vda5050::Node>& nodes,
                          const std::vector<vda5050::Edge>& edges);

  /// Clear order actions on new order; keeps active instant actions.
  void reset_for_new_order();

  // ─── Navigation event hooks ───────────────────────────────────────────────

  void on_node_reached(const std::string& node_id, uint32_t sequence_id);
  void on_edge_entered(const std::string& edge_id, uint32_t sequence_id);
  void on_edge_left(const std::string& edge_id, uint32_t sequence_id);

  // ─── Feedback from robot driver ───────────────────────────────────────────

  void set_action_running(const std::string& action_id);
  void set_action_finished(const std::string& action_id,
                           const std::string& result_description = "");
  void set_action_failed(const std::string& action_id,
                         const std::string& result_description = "");
  void set_action_paused(const std::string& action_id);

  // ─── Pause / Resume all ───────────────────────────────────────────────────

  void pause_all(const std::string& exclude_action_id = "");
  void resume_all(const std::string& exclude_action_id = "");
  void cancel_all(const std::string& exclude_action_id = "");

  // ─── State queries ────────────────────────────────────────────────────────

  /// Snapshot of all action states for the State message.
  std::vector<vda5050::ActionState> action_states() const;

  bool is_hard_blocked()    const;  ///< True if a HARD-blocking action is active.
  bool is_soft_blocked()    const;  ///< True if a SOFT-blocking action is active.
  bool has_active_actions() const;  ///< True if any action is INITIALIZING/RUNNING/PAUSED.
  /// True if any active action is bound to an order node/edge. Instant actions
  /// (cancelOrder, pause, stateRequest) are excluded, so they never block a
  /// replacing order.
  bool has_active_order_actions() const;

  void set_execute_callback(ActionExecuteCallback cb);
  void set_pause_callback(ActionPauseCallback cb);
  void set_resume_callback(ActionResumeCallback cb);
  void set_cancel_callback(ActionCancelCallback cb);

private:
  struct PendingCallbacks {
    ActionExecuteCallback         execute_cb;
    ActionPauseCallback           pause_cb;
    ActionResumeCallback          resume_cb;
    ActionCancelCallback          cancel_cb;
    std::vector<vda5050::Action>  execute_actions;
    std::vector<std::string>      pause_action_ids;
    std::vector<std::string>      resume_action_ids;
    std::vector<std::string>      cancel_action_ids;
  };

  enum class TriggerKind {
    IMMEDIATE,
    NODE_REACHED,
    EDGE_ENTERED
  };


  struct ActionRecord {
    vda5050::Action       action;
    vda5050::ActionStatus status{vda5050::ActionStatus::WAITING};
    std::string           result_description;
    bool                  is_instant{false};
    TriggerKind           trigger_kind{TriggerKind::IMMEDIATE};
    std::string           trigger_id;
    uint32_t              trigger_sequence_id{0};
    bool                  trigger_ready{false};
    bool                  pause_requested{false};
    bool                  resume_requested{false};
    bool                  paused_for_hard{false};
  };

  // ─── Internal helpers ─────────────────────────────────────────────────────

  void enqueue_actions_locked(const std::vector<vda5050::Action>& actions,
                              TriggerKind                         trigger_kind,
                              const std::string&                  trigger_id,
                              uint32_t                            trigger_sequence_id);

  void mark_trigger_ready_locked(TriggerKind         trigger_kind,
                                 const std::string&  trigger_id,
                                 uint32_t            trigger_sequence_id);

  void fail_edge_actions_locked(const std::string& edge_id,
                                uint32_t           sequence_id,
                                PendingCallbacks&  pending);

  void remove_stale_waiting_order_actions_locked(
    const std::unordered_map<std::string, ActionRecord>& desired_order_actions);

  void update_status(const std::string& action_id,
                     vda5050::ActionStatus new_status,
                     const std::string& result_desc = "");

  PendingCallbacks dispatch_pending_locked();
  static void invoke_pending_callbacks(const PendingCallbacks& pending);

  bool any_hard_running() const;
  bool any_soft_running() const;

  // ─── State ────────────────────────────────────────────────────────────────

  mutable std::mutex mutex_;

  std::vector<std::string>                              action_order_;
  std::unordered_map<std::string, ActionRecord>         actions_;
  bool                                                  dispatch_paused_{false};

  // ─── Callbacks ────────────────────────────────────────────────────────────

  ActionExecuteCallback on_execute_;
  ActionPauseCallback   on_pause_;
  ActionResumeCallback  on_resume_;
  ActionCancelCallback  on_cancel_;
};

}  // namespace vda5050_adapter
