#include "vda5050_client_adapter/action_manager.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

namespace vda5050_adapter 
{

namespace 
{

bool is_active_status(vda5050::ActionStatus status) 
{
  return status == vda5050::ActionStatus::INITIALIZING ||
         status == vda5050::ActionStatus::RUNNING ||
         status == vda5050::ActionStatus::PAUSED;
}

}  // namespace

ActionManager::ActionManager() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Callback registration
// ─────────────────────────────────────────────────────────────────────────────

void ActionManager::set_execute_callback(ActionExecuteCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_execute_ = std::move(cb);
}

void ActionManager::set_pause_callback(ActionPauseCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_pause_ = std::move(cb);
}

void ActionManager::set_resume_callback(ActionResumeCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_resume_ = std::move(cb);
}

void ActionManager::set_cancel_callback(ActionCancelCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_cancel_ = std::move(cb);
}

// ─────────────────────────────────────────────────────────────────────────────
// Action ingestion
// ─────────────────────────────────────────────────────────────────────────────

void ActionManager::enqueue_node_actions(const vda5050::Node& node) 
{
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    enqueue_actions_locked(
      node.actions, TriggerKind::NODE_REACHED, node.node_id, node.sequence_id);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::enqueue_edge_actions(const vda5050::Edge& edge) 
{
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    enqueue_actions_locked(
      edge.actions, TriggerKind::EDGE_ENTERED, edge.edge_id, edge.sequence_id);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::process_instant_actions(
  const vda5050::InstantActions& instant_actions)
{
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    enqueue_actions_locked(instant_actions.actions, TriggerKind::IMMEDIATE, "", 0);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::sync_order_actions(const std::vector<vda5050::Node>& nodes,
                                       const std::vector<vda5050::Edge>& edges) 
{
  struct TriggerSource 
  {
    TriggerKind                         kind;
    std::string                         trigger_id;
    uint32_t                            trigger_sequence_id;
    const std::vector<vda5050::Action>* actions;
  };

  std::vector<TriggerSource> sources;
  sources.reserve(nodes.size() + edges.size());

  for (const auto& node : nodes) 
  {
    sources.push_back(
      {
        TriggerKind::NODE_REACHED, node.node_id, node.sequence_id, &node.actions
      });
  }
  for (const auto& edge : edges) 
  {
    sources.push_back(
      {TriggerKind::EDGE_ENTERED, edge.edge_id, edge.sequence_id, &edge.actions});
  }

  std::sort(sources.begin(), sources.end(),[](const TriggerSource& lhs, const TriggerSource& rhs) {
              return lhs.trigger_sequence_id < rhs.trigger_sequence_id;
            });

  size_t expected_actions = 0;
  for (const auto& source : sources) 
  {
    expected_actions += source.actions->size();
  }

  std::unordered_map<std::string, ActionRecord> desired_order_actions;
  desired_order_actions.reserve(expected_actions);

  for (const auto& source : sources) 
  {
    for (const auto& action : *source.actions) 
    {
      if (desired_order_actions.count(action.action_id)) continue;

      ActionRecord rec;
      rec.action = action;
      rec.status = vda5050::ActionStatus::WAITING;
      rec.is_instant = false;
      rec.trigger_kind = source.kind;
      rec.trigger_id = source.trigger_id;
      rec.trigger_sequence_id = source.trigger_sequence_id;
      rec.trigger_ready = false;
      desired_order_actions.emplace(action.action_id, std::move(rec));
    }
  }

  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    remove_stale_waiting_order_actions_locked(desired_order_actions);

    for (const auto& source : sources) {
      for (const auto& action : *source.actions) {
        auto it = actions_.find(action.action_id);
        if (it == actions_.end()) {
          ActionRecord rec;
          rec.action = action;
          rec.status = vda5050::ActionStatus::WAITING;
          rec.is_instant = false;
          rec.trigger_kind = source.kind;
          rec.trigger_id = source.trigger_id;
          rec.trigger_sequence_id = source.trigger_sequence_id;
          rec.trigger_ready = false;

          action_order_.push_back(action.action_id);
          actions_.emplace(action.action_id, std::move(rec));
          continue;
        }

        auto& rec = it->second;
        if (rec.is_instant) continue;
        if (rec.status != vda5050::ActionStatus::WAITING || rec.trigger_ready) continue;

        rec.action = action;
        rec.trigger_kind = source.kind;
        rec.trigger_id = source.trigger_id;
        rec.trigger_sequence_id = source.trigger_sequence_id;
      }
    }

    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::reset_for_new_order() 
{
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> kept_order;
  std::unordered_map<std::string, ActionRecord> kept_actions;
  kept_order.reserve(action_order_.size());

  for (const auto& id : action_order_) 
  {
    auto it = actions_.find(id);
    if (it == actions_.end()) continue;

    const auto& rec = it->second;
    const bool keep_running_instant =
      rec.is_instant &&
      (rec.status == vda5050::ActionStatus::INITIALIZING ||
       rec.status == vda5050::ActionStatus::RUNNING);

    if (!keep_running_instant) continue;

    kept_order.push_back(id);
    kept_actions.emplace(id, rec);
  }

  action_order_ = std::move(kept_order);
  actions_ = std::move(kept_actions);
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation events
// ─────────────────────────────────────────────────────────────────────────────

void ActionManager::on_node_reached(const std::string& node_id,
                                    uint32_t           sequence_id) 
                                    {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mark_trigger_ready_locked(TriggerKind::NODE_REACHED, node_id, sequence_id);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::on_edge_entered(const std::string& edge_id,
                                    uint32_t           sequence_id) 
                                    {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mark_trigger_ready_locked(TriggerKind::EDGE_ENTERED, edge_id, sequence_id);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::on_edge_left(const std::string& edge_id,
                                 uint32_t           sequence_id) 
                                 {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending.cancel_cb = on_cancel_;
    fail_edge_actions_locked(edge_id, sequence_id, pending);

    auto dispatch_pending = dispatch_pending_locked();
    pending.execute_cb = dispatch_pending.execute_cb;
    pending.pause_cb = dispatch_pending.pause_cb;
    pending.resume_cb = dispatch_pending.resume_cb;
    if (!pending.cancel_cb) pending.cancel_cb = dispatch_pending.cancel_cb;
    pending.execute_actions.insert(pending.execute_actions.end(),
                                   dispatch_pending.execute_actions.begin(),
                                   dispatch_pending.execute_actions.end());
    pending.pause_action_ids.insert(pending.pause_action_ids.end(),
                                    dispatch_pending.pause_action_ids.begin(),
                                    dispatch_pending.pause_action_ids.end());
    pending.resume_action_ids.insert(pending.resume_action_ids.end(),
                                     dispatch_pending.resume_action_ids.begin(),
                                     dispatch_pending.resume_action_ids.end());
    pending.cancel_action_ids.insert(pending.cancel_action_ids.end(),
                                     dispatch_pending.cancel_action_ids.begin(),
                                     dispatch_pending.cancel_action_ids.end());
  }
  invoke_pending_callbacks(pending);
}

// ─────────────────────────────────────────────────────────────────────────────
// Feedback from robot driver
// ─────────────────────────────────────────────────────────────────────────────

void ActionManager::set_action_running(const std::string& action_id) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    update_status(action_id, vda5050::ActionStatus::RUNNING);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::set_action_finished(const std::string& action_id,
                                        const std::string& result_desc) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    update_status(action_id, vda5050::ActionStatus::FINISHED, result_desc);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::set_action_failed(const std::string& action_id,
                                      const std::string& result_desc) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    update_status(action_id, vda5050::ActionStatus::FAILED, result_desc);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::set_action_paused(const std::string& action_id) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    update_status(action_id, vda5050::ActionStatus::PAUSED);
    pending = dispatch_pending_locked();
  }
  invoke_pending_callbacks(pending);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pause / Resume / Cancel all
// ─────────────────────────────────────────────────────────────────────────────

void ActionManager::pause_all(const std::string& exclude_action_id) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatch_paused_ = true;
    pending.pause_cb = on_pause_;

    for (auto& [id, rec] : actions_) {
      if (id == exclude_action_id) continue;
      if (!is_active_status(rec.status)) continue;
      if (rec.pause_requested) continue;
      rec.pause_requested = true;
      rec.resume_requested = false;
      rec.paused_for_hard = false;
      pending.pause_action_ids.push_back(id);
    }
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::resume_all(const std::string& exclude_action_id) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatch_paused_ = false;
    pending.resume_cb = on_resume_;

    for (auto& [id, rec] : actions_) {
      if (id == exclude_action_id) continue;
      if (rec.status != vda5050::ActionStatus::PAUSED) continue;
      if (rec.resume_requested) continue;
      rec.resume_requested = true;
      pending.resume_action_ids.push_back(id);
    }

    auto dispatch_pending = dispatch_pending_locked();
    pending.execute_cb = dispatch_pending.execute_cb;
    pending.pause_cb = dispatch_pending.pause_cb;
    pending.resume_cb = dispatch_pending.resume_cb;
    pending.cancel_cb = dispatch_pending.cancel_cb;
    pending.execute_actions = std::move(dispatch_pending.execute_actions);
    pending.pause_action_ids.insert(pending.pause_action_ids.end(),
                                    dispatch_pending.pause_action_ids.begin(),
                                    dispatch_pending.pause_action_ids.end());
    pending.resume_action_ids.insert(pending.resume_action_ids.end(),
                                     dispatch_pending.resume_action_ids.begin(),
                                     dispatch_pending.resume_action_ids.end());
    pending.cancel_action_ids.insert(pending.cancel_action_ids.end(),
                                     dispatch_pending.cancel_action_ids.begin(),
                                     dispatch_pending.cancel_action_ids.end());
  }
  invoke_pending_callbacks(pending);
}

void ActionManager::cancel_all(const std::string& exclude_action_id) {
  PendingCallbacks pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending.cancel_cb = on_cancel_;

    for (auto& [id, rec] : actions_) {
      if (id == exclude_action_id) continue;
      if (rec.status == vda5050::ActionStatus::FINISHED ||
          rec.status == vda5050::ActionStatus::FAILED) 
          {
        continue;
      }

      const bool was_active = is_active_status(rec.status);
      rec.status = vda5050::ActionStatus::FAILED;
      rec.result_description = "Cancelled";
      rec.pause_requested = false;
      rec.resume_requested = false;
      rec.paused_for_hard = false;

      if (was_active) pending.cancel_action_ids.push_back(id);
    }
  }
  invoke_pending_callbacks(pending);
}

// ─────────────────────────────────────────────────────────────────────────────
// State queries
// ─────────────────────────────────────────────────────────────────────────────

std::vector<vda5050::ActionState> ActionManager::action_states() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::ActionState> result;
  result.reserve(action_order_.size());

  for (const auto& id : action_order_) {
    auto it = actions_.find(id);

    if (it == actions_.end()) continue;

    const auto& rec = it->second;
    vda5050::ActionState as;
    as.action_id = rec.action.action_id;
    as.action_type = rec.action.action_type;
    as.action_description = rec.action.action_description;
    as.action_status = rec.status;
    as.result_description = rec.result_description;
    result.push_back(std::move(as));
  }

  return result;
}

bool ActionManager::is_hard_blocked() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return any_hard_running();
}

bool ActionManager::is_soft_blocked() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return any_soft_running();
}

bool ActionManager::has_active_actions() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [id, rec] : actions_) 
  {
    if (is_active_status(rec.status)) return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

void ActionManager::enqueue_actions_locked(
  const std::vector<vda5050::Action>& actions,
  TriggerKind                         trigger_kind,
  const std::string&                  trigger_id,
  uint32_t                            trigger_sequence_id)
{
  for (const auto& action : actions) {
    if (actions_.count(action.action_id)) continue;

    ActionRecord rec;
    rec.action = action;
    rec.status = vda5050::ActionStatus::WAITING;
    rec.is_instant = (trigger_kind == TriggerKind::IMMEDIATE);
    rec.trigger_kind = trigger_kind;
    rec.trigger_id = trigger_id;
    rec.trigger_sequence_id = trigger_sequence_id;
    rec.trigger_ready = rec.is_instant;

    action_order_.push_back(action.action_id);
    actions_[action.action_id] = std::move(rec);
  }
}

void ActionManager::mark_trigger_ready_locked(TriggerKind        trigger_kind,
                                              const std::string& trigger_id,
                                              uint32_t           trigger_sequence_id) {
  for (auto& [id, rec] : actions_) {
    if (rec.trigger_kind != trigger_kind) continue;
    if (rec.trigger_id != trigger_id) continue;
    if (rec.trigger_sequence_id != trigger_sequence_id) continue;
    rec.trigger_ready = true;
  }
}

void ActionManager::fail_edge_actions_locked(const std::string& edge_id,
                                             uint32_t           sequence_id,
                                             PendingCallbacks&  pending) {
  for (auto& [id, rec] : actions_) {
    if (rec.trigger_kind != TriggerKind::EDGE_ENTERED) continue;
    if (rec.trigger_id != edge_id) continue;
    if (rec.trigger_sequence_id != sequence_id) continue;
    if (rec.status == vda5050::ActionStatus::FINISHED ||
        rec.status == vda5050::ActionStatus::FAILED) {
      continue;
    }

    const bool was_active = is_active_status(rec.status);
    rec.status = vda5050::ActionStatus::FAILED;
    rec.result_description = "Edge left before action completed";

    if (was_active) pending.cancel_action_ids.push_back(id);
  }
}

void ActionManager::remove_stale_waiting_order_actions_locked(
  const std::unordered_map<std::string, ActionRecord>& desired_order_actions) {
  std::vector<std::string> compact_order;
  compact_order.reserve(action_order_.size());

  for (const auto& id : action_order_) {
    auto it = actions_.find(id);
    if (it == actions_.end()) continue;

    const auto remove_stale_waiting_order_action =
      !it->second.is_instant &&
      it->second.status == vda5050::ActionStatus::WAITING &&
      !it->second.trigger_ready &&
      !desired_order_actions.count(id);

    if (remove_stale_waiting_order_action) {
      actions_.erase(it);
      continue;
    }

    compact_order.push_back(id);
  }

  action_order_ = std::move(compact_order);
}

void ActionManager::update_status(const std::string&    action_id,
                                  vda5050::ActionStatus new_status,
                                  const std::string&    result_desc) {
  auto it = actions_.find(action_id);
  if (it == actions_.end()) {
    std::cerr << "[ActionManager] Unknown actionId: " << action_id << "\n";
    return;
  }

  auto& rec = it->second;
  rec.status = new_status;
  rec.result_description = result_desc;

  if (new_status == vda5050::ActionStatus::PAUSED) {
    rec.pause_requested = false;
    rec.resume_requested = false;
    return;
  }

  rec.pause_requested = false;
  rec.resume_requested = false;
  if (new_status == vda5050::ActionStatus::RUNNING ||
      new_status == vda5050::ActionStatus::FINISHED ||
      new_status == vda5050::ActionStatus::FAILED) {
    rec.paused_for_hard = false;
  }
}

ActionManager::PendingCallbacks ActionManager::dispatch_pending_locked() {
  PendingCallbacks pending;
  pending.execute_cb = on_execute_;
  pending.pause_cb = on_pause_;
  pending.resume_cb = on_resume_;
  pending.cancel_cb = on_cancel_;

  bool hard_action_waiting_to_run = false;
  for (const auto& [id, rec] : actions_) {
    (void)id;
    if (rec.action.blocking_type != vda5050::BlockingType::HARD) continue;
    if (rec.status != vda5050::ActionStatus::WAITING) continue;
    if (!rec.trigger_ready) continue;
    hard_action_waiting_to_run = true;
    break;
  }

  if (!dispatch_paused_ && !any_hard_running() && !hard_action_waiting_to_run) {
    for (auto& [id, rec] : actions_) {
      if (rec.status != vda5050::ActionStatus::PAUSED) continue;
      if (!rec.paused_for_hard) continue;
      if (rec.resume_requested) continue;
      rec.resume_requested = true;
      pending.resume_action_ids.push_back(id);
    }
  }

  for (const auto& id : action_order_) {
    auto it = actions_.find(id);
    if (it == actions_.end()) continue;
    auto& rec = it->second;

    if (rec.status != vda5050::ActionStatus::WAITING) continue;
    if (!rec.trigger_ready) continue;
    if (dispatch_paused_ && !rec.is_instant) continue;
    if (any_hard_running()) continue;

    if (rec.action.blocking_type == vda5050::BlockingType::HARD) {
      bool has_unpaused_other_action = false;
      for (auto& [other_id, other] : actions_) {
        if (other_id == id) continue;
        if (other.status != vda5050::ActionStatus::INITIALIZING &&
            other.status != vda5050::ActionStatus::RUNNING) {
          continue;
        }

        has_unpaused_other_action = true;
        if (other.pause_requested) continue;
        other.pause_requested = true;
        other.resume_requested = false;
        other.paused_for_hard = true;
        pending.pause_action_ids.push_back(other_id);
      }

      if (has_unpaused_other_action) continue;
    }

    rec.status = vda5050::ActionStatus::INITIALIZING;
    rec.result_description.clear();
    rec.pause_requested = false;
    rec.resume_requested = false;
    rec.paused_for_hard = false;
    pending.execute_actions.push_back(rec.action);
  }

  return pending;
}

void ActionManager::invoke_pending_callbacks(const PendingCallbacks& pending) {
  if (pending.pause_cb) {
    for (const auto& id : pending.pause_action_ids) pending.pause_cb(id);
  }

  if (pending.resume_cb) {
    for (const auto& id : pending.resume_action_ids) pending.resume_cb(id);
  }

  if (pending.cancel_cb) {
    for (const auto& id : pending.cancel_action_ids) pending.cancel_cb(id);
  }

  if (pending.execute_cb) {
    for (const auto& action : pending.execute_actions) pending.execute_cb(action);
  }
}

bool ActionManager::any_hard_running() const {
  for (const auto& [id, rec] : actions_) {
    if (rec.action.blocking_type == vda5050::BlockingType::HARD &&
        (rec.status == vda5050::ActionStatus::RUNNING ||
         rec.status == vda5050::ActionStatus::INITIALIZING)) {
      return true;
    }
  }
  return false;
}

bool ActionManager::any_soft_running() const {
  for (const auto& [id, rec] : actions_) {
    if (rec.action.blocking_type == vda5050::BlockingType::SOFT &&
        (rec.status == vda5050::ActionStatus::RUNNING ||
         rec.status == vda5050::ActionStatus::INITIALIZING)) {
      return true;
    }
  }
  return false;
}

}  // namespace vda5050_client_adapter
