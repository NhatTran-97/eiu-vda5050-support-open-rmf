#pragma once

#include <chrono>
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
 * @brief Manages VDA5050 action lifecycle: execution, blocking semantics, pause/resume/cancel.
 *
 * Tracks actions from order routes and instantActions, manages their state transitions
 * (WAITING → INITIALIZING → RUNNING → FINISHED|FAILED, with PAUSED reachable from
 * INITIALIZING/RUNNING), and enforces blocking constraints (NONE/SOFT/HARD).
 *
 * Blocking types:
 *   - NONE:  Runs concurrently with other actions; does not block driving.
 *   - SOFT:  Driving stops; NONE actions may run in parallel; other SOFT/HARD wait.
 *   - HARD:  Pauses all other actions; runs alone; resumes others when finished.
 *
 * Key responsibilities:
 *  - Ingest and queue node/edge/instant actions with their trigger conditions
 *  - Manage action lifecycle state transitions and validate transitions
 *  - Enforce blocking semantics: HARD exclusive, SOFT stop-driving, NONE parallel
 *  - Dispatch callbacks (execute, pause, resume, cancel) to robot driver
 *  - Handle pause/resume/cancel requests on all active actions
 *  - Timeout HARD actions waiting too long for pause confirmation
 *  - Thread-safe: all public methods callable from any thread
 */
class ActionManager {
public:
  // ─── Callbacks ────────────────────────────────────────────────────────────

  /**
   * @brief Callback to start executing an action.
   * @param action The action to execute.
   */
  using ActionExecuteCallback =
    std::function<void(const vda5050::Action&)>;

  /**
   * @brief Callback to pause a running action.
   * @param action_id ID of the action to pause.
   */
  using ActionPauseCallback =
    std::function<void(const std::string& action_id)>;

  /**
   * @brief Callback to resume a paused action.
   * @param action_id ID of the action to resume.
   */
  using ActionResumeCallback =
    std::function<void(const std::string& action_id)>;

  /**
   * @brief Callback to cancel an action.
   * @param action_id ID of the action to cancel.
   */
  using ActionCancelCallback =
    std::function<void(const std::string& action_id)>;

  /**
   * @brief Construct an action manager with no callbacks wired yet.
   */
  ActionManager();

  /**
   * @brief Destructor.
   */
  ~ActionManager() = default;

  // ─── Action ingestion ─────────────────────────────────────────────────────

  // Queue node (node)'s actions; dispatched when the node is reached.
  void enqueue_node_actions(const vda5050::Node& node);

  // Queue edge (edge)'s actions; dispatched when the edge is entered.
  void enqueue_edge_actions(const vda5050::Edge& edge);

  // Execute instantActions (instant_actions) immediately without waiting for navigation.
  void process_instant_actions(const vda5050::InstantActions& instant_actions);

  // Sync order actions against nodes/edges (nodes, edges); drop stale actions, add new ones.
  void sync_order_actions(const std::vector<vda5050::Node>& nodes,
                          const std::vector<vda5050::Edge>& edges);

  // Clear all order actions on new order; keeps active instant actions running.
  void reset_for_new_order();

  // ─── Navigation event hooks ───────────────────────────────────────────────

  // Mark node (node_id, sequence_id) as reached; trigger actions attached to this node.
  void on_node_reached(const std::string& node_id, uint32_t sequence_id);
  // Mark edge (edge_id, sequence_id) as entered; trigger actions attached to this edge.
  void on_edge_entered(const std::string& edge_id, uint32_t sequence_id);
  // Mark edge (edge_id, sequence_id) as exited; fail hanging edge actions if not FINISHED/FAILED.
  void on_edge_left(const std::string& edge_id, uint32_t sequence_id);

  // ─── Feedback from robot driver ───────────────────────────────────────────

  // Update action (action_id) status to RUNNING; dispatch pause if PAUSED requested.
  void set_action_running(const std::string& action_id);
  // Complete action (action_id) with FINISHED status and result_description.
  void set_action_finished(const std::string& action_id,
                           const std::string& result_description = "");
  // Complete action (action_id) with FAILED status and result_description.
  void set_action_failed(const std::string& action_id,
                         const std::string& result_description = "");
  // Mark action (action_id) as PAUSED.
  void set_action_paused(const std::string& action_id);

  // ─── Pause / Resume all ───────────────────────────────────────────────────

  // Pause all active actions except exclude_action_id; queue pause callbacks.
  void pause_all(const std::string& exclude_action_id = "");
  // Resume all paused actions except exclude_action_id; queue resume callbacks.
  void resume_all(const std::string& exclude_action_id = "");
  // Cancel all active actions except exclude_action_id; queue cancel callbacks.
  void cancel_all(const std::string& exclude_action_id = "");

  // Check HARD action timeouts (now, hard_pause_timeout): fail actions waiting too long for pause confirmation.
  bool check_timeouts(std::chrono::steady_clock::time_point now,
                      std::chrono::steady_clock::duration   hard_pause_timeout);

  // ─── State queries ────────────────────────────────────────────────────────

  // Snapshot of all action states for State message publication.
  std::vector<vda5050::ActionState> action_states() const;

  // Return true if a HARD-blocking action is currently active.
  bool is_hard_blocked()    const;
  // Return true if a SOFT-blocking action is currently active.
  bool is_soft_blocked()    const;
  // Return true if any action is INITIALIZING/RUNNING/PAUSED.
  bool has_active_actions() const;
  // Return true if any order action (not instant action) is active.
  bool has_active_order_actions() const;

  // Set callback to execute when action (action) should start.
  void set_execute_callback(ActionExecuteCallback cb);
  // Set callback to pause action by id (action_id).
  void set_pause_callback(ActionPauseCallback cb);
  // Set callback to resume action by id (action_id).
  void set_resume_callback(ActionResumeCallback cb);
  // Set callback to cancel action by id (action_id).
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
    // Set while this HARD action is blocked waiting for another to confirm it paused; cleared once it dispatches or stops being blocked. Basis for check_timeouts().
    std::optional<std::chrono::steady_clock::time_point> hard_wait_since;
  };

  // ─── Internal helpers ─────────────────────────────────────────────────────

  // Queue actions (actions) with trigger type/id/sequence; dispatch callbacks if ready (mutex held).
  void enqueue_actions_locked(const std::vector<vda5050::Action>& actions,
                              TriggerKind                         trigger_kind,
                              const std::string&                  trigger_id,
                              uint32_t                            trigger_sequence_id);

  // Mark trigger (trigger_kind, trigger_id, trigger_sequence_id) ready; dispatch affected actions (mutex held).
  void mark_trigger_ready_locked(TriggerKind         trigger_kind,
                                 const std::string&  trigger_id,
                                 uint32_t            trigger_sequence_id);

  // Fail all edge (edge_id, sequence_id) actions with FAILED status; queue cancel callbacks (mutex held).
  void fail_edge_actions_locked(const std::string& edge_id,  uint32_t           sequence_id, PendingCallbacks&  pending);

  // Remove order actions not in desired_order_actions (desired_order_actions); preserve instant/active (mutex held).
  void remove_stale_waiting_order_actions_locked(const std::unordered_map<std::string, ActionRecord>& desired_order_actions);

  // Update action (action_id) status to new_status with optional result_desc (mutex held).
  void update_status(const std::string& action_id, vda5050::ActionStatus new_status, const std::string& result_desc = "");

  // Collect pending callbacks from active actions; clear queues (mutex held).
  PendingCallbacks dispatch_pending_locked();
  // Invoke all callbacks in pending (pending) (static, thread-safe).
  static void invoke_pending_callbacks(const PendingCallbacks& pending);

  // Return true if any HARD-blocking action is INITIALIZING/RUNNING/PAUSED (mutex held).
  bool any_hard_running() const;
  // Return true if any SOFT-blocking action is INITIALIZING/RUNNING/PAUSED (mutex held).
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
