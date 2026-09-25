#include "vda5050_client_adapter/order_manager.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include <rclcpp/logging.hpp>

namespace vda5050_adapter {

namespace {

rclcpp::Logger logger()
{
  return rclcpp::get_logger("vda5050_client_adapter.order_manager");
}

// Order node/edge states along the route.
template <typename State>
bool by_sequence_id(const State& lhs, const State& rhs)
{
  return lhs.sequence_id < rhs.sequence_id;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

OrderManager::OrderManager() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Callback registration
// ─────────────────────────────────────────────────────────────────────────────

// Register callback (cb) to invoke when an order is accepted; passes order_id, order_update_id, and remaining route.
void OrderManager::set_order_accepted_callback(OrderAcceptedCallback cb)
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_order_accepted_ = std::move(cb);
}

// Register callback (cb) to invoke when an order is cancelled; passes cancelled order_id.
void OrderManager::set_order_cancelled_callback(OrderCancelledCallback cb)
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_order_cancelled_ = std::move(cb);
}

// Register callback (cb) to invoke when the AGV should request a new base segment (newBaseRequest flag).
void OrderManager::set_new_base_request_callback(NewBaseRequestCallback cb)
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_new_base_request_ = std::move(cb);
}

// ─────────────────────────────────────────────────────────────────────────────
// Process incoming order
// ─────────────────────────────────────────────────────────────────────────────

// Validate and apply incoming order (order): new order, update of the current one, or a duplicate to ignore.
OrderAcceptResult OrderManager::process_order(const vda5050::Order& order)
{
  std::unique_lock<std::mutex> lock(mutex_);

  if (strict_mode_)
  {
    const std::string structure_error = validate_structure(order);
    if (!structure_error.empty())
    {
      return OrderAcceptResult::rejected("validationError", structure_error);
    }
  }

  if (order.order_id != current_order_id_)
  {
    const auto result = validate_new_order(order);
    if (!result.accepted) return result;
    apply_order(order);
  }
  else
  {
    if (order.order_update_id == current_order_update_id_)
    {
      return OrderAcceptResult::ignored_duplicate();
    }
    if (order.order_update_id < current_order_update_id_)
    {
      if (!strict_mode_)
      {
        return OrderAcceptResult::rejected("orderError", "orderUpdateId must be greater than current (" + std::to_string(current_order_update_id_) + ")");
      }
      return OrderAcceptResult::rejected("orderUpdateError",
        "orderUpdateId " + std::to_string(order.order_update_id) + " is lower than the current " + std::to_string(current_order_update_id_));
    }
    const auto result = validate_update(order);
    if (!result.accepted) return result;
    apply_stitch(order);
  }

  auto accepted_cb = on_order_accepted_;
  std::vector<vda5050::Node> remaining_nodes(remaining_base_nodes_.begin(), remaining_base_nodes_.end());
  std::vector<vda5050::Edge> remaining_edges(remaining_base_edges_.begin(), remaining_base_edges_.end());
  remaining_nodes.insert(remaining_nodes.end(), horizon_nodes_.begin(), horizon_nodes_.end());
  remaining_edges.insert(remaining_edges.end(), horizon_edges_.begin(), horizon_edges_.end());
  lock.unlock();

  if (accepted_cb)
  {
    accepted_cb(order.order_id, order.order_update_id, remaining_nodes, remaining_edges);
  }
  return OrderAcceptResult::ok();
}

// Check node/edge alternation, sequence ids, edge endpoints, base/horizon split and unique ids.
std::string OrderManager::validate_structure(const vda5050::Order& order)
{
  if (order.order_id.empty())
  {
    return "orderId must not be empty";
  }
  if (order.nodes.empty())
  {
    return "Order must contain at least one node";
  }
  if (order.edges.size() + 1 != order.nodes.size())
  {
    return "Order must have exactly one edge less than nodes (nodes=" +
           std::to_string(order.nodes.size()) + ", edges=" + std::to_string(order.edges.size()) + ")";
  }
  if (!order.nodes.front().released)
  {
    return "First node '" + order.nodes.front().node_id + "' must be released";
  }

  std::set<std::string> action_ids;
  const auto check_actions = [&](const std::vector<vda5050::Action>& actions,
                                 const std::string& owner) -> std::string {
    for (const auto& action : actions)
    {
      if (action.action_id.empty())
      {
        return "Action on " + owner + " has an empty actionId";
      }
      if (!action_ids.insert(action.action_id).second)
      {
        return "actionId '" + action.action_id + "' is used more than once";
      }
    }
    return "";
  };

  for (std::size_t i = 0; i < order.nodes.size(); ++i)
  {
    const auto& node = order.nodes[i];
    if (node.node_id.empty())
    {
      return "Node at index " + std::to_string(i) + " has an empty nodeId";
    }
    auto error = check_actions(node.actions, "node '" + node.node_id + "'");
    if (!error.empty()) return error;
    if (i + 1 == order.nodes.size()) break;

    const auto& edge = order.edges[i];
    const auto& next = order.nodes[i + 1];
    if (edge.edge_id.empty())
    {
      return "Edge at index " + std::to_string(i) + " has an empty edgeId";
    }
    if (edge.sequence_id != node.sequence_id + 1 || next.sequence_id != node.sequence_id + 2)
    {
      return "sequenceId must increase by one along the route (node '" + node.node_id + "'=" +
             std::to_string(node.sequence_id) + ", edge '" + edge.edge_id + "'=" +
             std::to_string(edge.sequence_id) + ", node '" + next.node_id + "'=" +
             std::to_string(next.sequence_id) + ")";
    }
    if (edge.start_node_id != node.node_id || edge.end_node_id != next.node_id)
    {
      return "Edge '" + edge.edge_id + "' must connect '" + node.node_id + "' to '" + next.node_id + "'";
    }
    if (next.released && !node.released)
    {
      return "Released node '" + next.node_id + "' follows a horizon node";
    }
    if (edge.released != next.released)
    {
      return "Edge '" + edge.edge_id + "' must have the released flag of its end node '" + next.node_id + "'";
    }
    error = check_actions(edge.actions, "edge '" + edge.edge_id + "'");
    if (!error.empty()) return error;
  }
  return "";
}

// Switch between strict VDA5050 order handling and the default behavior (see header).
void OrderManager::set_strict_mode(bool strict)
{
  std::lock_guard<std::mutex> lock(mutex_);
  strict_mode_ = strict;
}

// Set how many released nodes must remain before newBaseRequest is raised.
void OrderManager::set_new_base_request_min_base_nodes(std::size_t min_base_nodes)
{
  std::lock_guard<std::mutex> lock(mutex_);
  new_base_request_min_base_nodes_ = min_base_nodes;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cancel
// ─────────────────────────────────────────────────────────────────────────────

// Cancel the active order (order_id); if order_id is non-empty, only cancel if it matches current order. Invokes cancelled callback.
void OrderManager::cancel_order(const std::string& order_id)
{
  std::unique_lock<std::mutex> lock(mutex_);

  if (!order_id.empty() && order_id != current_order_id_) {
    RCLCPP_WARN(logger(), "cancel_order: id mismatch ('%s' vs '%s')", order_id.c_str(), current_order_id_.c_str());
    return;
  }

  std::string cancelled_id = current_order_id_;

  remaining_base_nodes_.clear();
  remaining_base_edges_.clear();
  horizon_nodes_.clear();
  horizon_edges_.clear();
  active_edges_.clear();
  if (!strict_mode_)
  {
    current_order_id_        = "";
    current_order_update_id_ = 0;
    current_zone_set_id_     = "";
  }
  order_active_             = false;
  new_base_request_         = false;
  distance_since_last_node_ = 0.0;

  auto cancel_cb = on_order_cancelled_;
  lock.unlock();

  if (cancel_cb) cancel_cb(cancelled_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation feedback
// ─────────────────────────────────────────────────────────────────────────────

// First released node not yet reached and its incoming edge (entered or still pending).
std::optional<RouteStep> OrderManager::next_step() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!order_active_ || remaining_base_nodes_.empty()) 
  {
    return std::nullopt;
  }

  RouteStep step;
  step.order_id        = current_order_id_;
  step.order_update_id = current_order_update_id_;
  step.node            = remaining_base_nodes_.front();
  if (step.node.sequence_id > 0) {
    const uint32_t edge_seq = step.node.sequence_id - 1;
    const auto matches = [edge_seq](const vda5050::Edge& e) { return e.sequence_id == edge_seq; };
    const auto active_it = std::find_if(active_edges_.begin(), active_edges_.end(), matches);
    if (active_it != active_edges_.end()) 
    {
      step.incoming_edge = *active_it;
      step.edge_entered  = true;
    } else if (!remaining_base_edges_.empty() && matches(remaining_base_edges_.front())) 
    {
      step.incoming_edge = remaining_base_edges_.front();
    }
  }
  return step;
}

// Pops the next base node when it is evt's node; raises newBaseRequest when the base runs low.
bool OrderManager::node_reached(const NodeReachedEvent& evt) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!order_active_ || remaining_base_nodes_.empty()) 
  {
    RCLCPP_WARN(logger(), "Unexpected node_reached '%s': no released base node pending", evt.node_id.c_str());
    return false;
  }

  const auto& expected_node = remaining_base_nodes_.front();
  if (expected_node.node_id != evt.node_id || expected_node.sequence_id != evt.sequence_id) 
      {
    RCLCPP_WARN(logger(), "Out-of-order node_reached: expected '%s' (seq=%u), got '%s' (seq=%u)",
                expected_node.node_id.c_str(), expected_node.sequence_id, evt.node_id.c_str(), evt.sequence_id);
    return false;
  }

  last_node_id_            = evt.node_id;
  last_node_sequence_id_   = evt.sequence_id;
  distance_since_last_node_ = evt.distance_driven;

  remaining_base_nodes_.pop_front();

  // Route fully consumed — order done.
  if (remaining_base_nodes_.empty() && horizon_nodes_.empty()) 
  {
    order_active_ = false;
  }

  const bool should_request = remaining_base_nodes_.size() < new_base_request_min_base_nodes_ && !horizon_nodes_.empty();
  const bool notify_new_base_request = should_request && !new_base_request_;
  new_base_request_ = should_request;

  auto new_base_cb = notify_new_base_request ? on_new_base_request_ : NewBaseRequestCallback{};

  lock.unlock();
  if (new_base_cb) new_base_cb();
  return true;
}

// Moves the next base edge to the active edges when it is (edge_id, sequence_id).
bool OrderManager::edge_entered(const std::string& edge_id, uint32_t sequence_id)
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (remaining_base_edges_.empty()) {
    RCLCPP_WARN(logger(), "Unexpected edge_entered '%s': no released base edge pending", edge_id.c_str());
    return false;
  }

  const auto& expected_edge = remaining_base_edges_.front();
  if (expected_edge.edge_id != edge_id || expected_edge.sequence_id != sequence_id) 
  {
    RCLCPP_WARN(logger(), "Out-of-order edge_entered: expected '%s' (seq=%u), got '%s' (seq=%u)",
                expected_edge.edge_id.c_str(), expected_edge.sequence_id, edge_id.c_str(), sequence_id);
    return false;
  }

  active_edges_.push_back(expected_edge);
  remaining_base_edges_.pop_front();
  return true;
}

// Removes the active edge (edge_id, sequence_id).
bool OrderManager::edge_completed(const std::string& edge_id, uint32_t sequence_id)
{
  std::lock_guard<std::mutex> lock(mutex_);

  const auto active_it = std::find_if(active_edges_.begin(), active_edges_.end(), [&](const vda5050::Edge& e) 
  {
    return e.edge_id == edge_id && e.sequence_id == sequence_id;
  });
  if (active_it == active_edges_.end()) 
  {
    RCLCPP_WARN(logger(), "Unexpected edge_completed '%s' (seq=%u): edge not entered", edge_id.c_str(), sequence_id);
    return false;
  }
  active_edges_.erase(active_it);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// State queries
// ─────────────────────────────────────────────────────────────────────────────

// Return current order id; empty if no active order.
std::string OrderManager::current_order_id() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return current_order_id_;
}

// Return current order update id; 0 if no active order.
uint32_t OrderManager::current_order_update_id() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return current_order_update_id_;
}

// Return id of last physically reached node; empty if none yet reached.
std::string OrderManager::last_node_id() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return last_node_id_;
}

// Return sequence_id of last physically reached node; 0 if none yet reached.
uint32_t OrderManager::last_node_sequence_id() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return last_node_sequence_id_;
}

// Return zone set id from current order; empty if no active order.
std::string OrderManager::current_zone_set_id() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return current_zone_set_id_;
}

// Return distance (meters) driven since last node reached.
double OrderManager::distance_since_last_node() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return distance_since_last_node_;
}

// Update the live distance-since-last-node reading (see header).
void OrderManager::set_distance_since_last_node(double meters)
{
  std::lock_guard<std::mutex> lock(mutex_);
  distance_since_last_node_ = meters;
}

// Return snapshot of all remaining node states (base + horizon) for State message.
std::vector<vda5050::NodeState> OrderManager::node_states() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::NodeState> result;
  result.reserve(remaining_base_nodes_.size() + horizon_nodes_.size());
  for (const auto& n : remaining_base_nodes_) result.push_back(node_to_state(n));
  for (const auto& n : horizon_nodes_)        result.push_back(node_to_state(n));
  std::stable_sort(result.begin(), result.end(), by_sequence_id<decltype(result)::value_type>);
  return result;
}

// Return snapshot of all edge states (active + remaining base + horizon) for State message.
std::vector<vda5050::EdgeState> OrderManager::edge_states() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::EdgeState> result;
  result.reserve(active_edges_.size() + remaining_base_edges_.size() + horizon_edges_.size());
  for (const auto& e : active_edges_)         result.push_back(edge_to_state(e));
  for (const auto& e : remaining_base_edges_) result.push_back(edge_to_state(e));
  for (const auto& e : horizon_edges_)        result.push_back(edge_to_state(e));
  std::stable_sort(result.begin(), result.end(), by_sequence_id<decltype(result)::value_type>);
  return result;
}

// Return snapshot of currently active edges (entered but not yet completed).
std::vector<vda5050::EdgeState> OrderManager::active_edge_states() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::EdgeState> result;
  result.reserve(active_edges_.size());
  for (const auto& e : active_edges_) result.push_back(edge_to_state(e));
  return result;
}

// Return true if AGV should request a new base segment (remaining base < 2 nodes).
bool OrderManager::new_base_request() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return new_base_request_;
}

// Return true if there is an active order in progress.
bool OrderManager::has_active_order() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return order_active_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// Validate new order (order): in strict mode refused while an order is active; otherwise a
// replacing order must start at the last traversed node.
OrderAcceptResult
OrderManager::validate_new_order(const vda5050::Order& order) const
{
  if (order.nodes.empty())
  {
    return OrderAcceptResult::rejected("orderError", "Order must contain at least one node");
  }
  if (!order_active_)
  {
    return OrderAcceptResult::ok();
  }
  if (strict_mode_)
  {
    return OrderAcceptResult::rejected("orderError", "Order '" + current_order_id_ + "' is still active");
  }
  // A new order_id supersedes the active one: only the node the robot is standing on must match
  // (sequence_id resets per order).
  if (!last_node_id_.empty() && order.nodes.front().node_id != last_node_id_)
  {
    return OrderAcceptResult::rejected("orderError",
      "New order's first node must match last traversed node (id=" + last_node_id_ + ")");
  }
  return OrderAcceptResult::ok();
}

// Validate order update (update): the first node must be the base end -- the last released node,
// or the last traversed node once the base is used up. Outside strict mode the horizon end is
// accepted as well.
OrderAcceptResult
OrderManager::validate_update(const vda5050::Order& update) const
{
  const char* error_type = strict_mode_ ? "orderUpdateError" : "orderError";
  if (update.nodes.empty())
  {
    return OrderAcceptResult::rejected(error_type, "Order update must contain at least one node");
  }

  const auto& stitch_node = update.nodes.front();
  const std::string base_end_id = remaining_base_nodes_.empty() ? last_node_id_ : remaining_base_nodes_.back().node_id;
  const uint32_t base_end_seq = remaining_base_nodes_.empty() ? last_node_sequence_id_ : remaining_base_nodes_.back().sequence_id;

  bool stitch_ok = stitch_node.node_id == base_end_id && stitch_node.sequence_id == base_end_seq;
  if (!stitch_ok && !strict_mode_ && !horizon_nodes_.empty())
  {
    stitch_ok = stitch_node.node_id == horizon_nodes_.back().node_id && stitch_node.sequence_id == horizon_nodes_.back().sequence_id;
  }
  if (!stitch_ok)
  {
    return OrderAcceptResult::rejected(error_type,
      "Order update stitch node mismatch: expected node_id=" + base_end_id + " sequenceId=" + std::to_string(base_end_seq));
  }
  return OrderAcceptResult::ok();
}

// Apply new order (order): set order_id/update_id/zone_set_id, partition nodes/edges into base/horizon, reset progress.
void OrderManager::apply_order(const vda5050::Order& order)
{
  current_order_id_        = order.order_id;
  current_order_update_id_ = order.order_update_id;
  current_zone_set_id_     = order.zone_set_id;
  order_active_            = true;
  new_base_request_        = false;
  distance_since_last_node_ = 0.0;

  // lastNodeSequenceId restarts per order: the master derives route progress from it.
  last_node_sequence_id_ = 0;

  remaining_base_nodes_.clear();
  remaining_base_edges_.clear();
  horizon_nodes_.clear();
  horizon_edges_.clear();
  active_edges_.clear();

  for (const auto& n : order.nodes)
  {
    if (n.released) remaining_base_nodes_.push_back(n);
    else            horizon_nodes_.push_back(n);
  }
  for (const auto& e : order.edges)
  {
    if (e.released) remaining_base_edges_.push_back(e);
    else            horizon_edges_.push_back(e);
  }

  // lastNodeId is only updated when the robot physically reaches a node, not on order accept.
}

// Apply order update (update): replace the horizon from the stitch node on and append the new nodes/edges.
void OrderManager::apply_stitch(const vda5050::Order& update) {
  current_order_update_id_ = update.order_update_id;
  new_base_request_        = false;

  const auto& stitch = update.nodes.front();
  const bool stitch_at_base_end =
    remaining_base_nodes_.empty()
      ? last_node_id_ == stitch.node_id && last_node_sequence_id_ == stitch.sequence_id : remaining_base_nodes_.back().node_id == stitch.node_id &&
        remaining_base_nodes_.back().sequence_id == stitch.sequence_id;

  if (stitch_at_base_end)
  {
    horizon_nodes_.clear();
    horizon_edges_.clear();
  }
  else
  {
    // Horizon-end stitch: keep the horizon before the stitch node, the update replaces the rest.
    const auto from_stitch_on = [&](const auto& element) { return element.sequence_id >= stitch.sequence_id; };
    horizon_nodes_.erase(std::remove_if(horizon_nodes_.begin(), horizon_nodes_.end(), from_stitch_on), horizon_nodes_.end());
    horizon_edges_.erase(std::remove_if(horizon_edges_.begin(), horizon_edges_.end(), from_stitch_on), horizon_edges_.end());
  }

  for (std::size_t i = stitch_at_base_end ? 1u : 0u; i < update.nodes.size(); ++i)
  {
    const auto& n = update.nodes[i];
    if (n.released) remaining_base_nodes_.push_back(n);
    else            horizon_nodes_.push_back(n);
  }
  for (const auto& e : update.edges)
  {
    if (e.released) remaining_base_edges_.push_back(e);
    else            horizon_edges_.push_back(e);
  }

  if (!remaining_base_nodes_.empty() || !horizon_nodes_.empty())
  {
    order_active_ = true;
  }
}

// ─── Static converters ────────────────────────────────────────────────────────

// Convert Node (n) to NodeState for State message publication.
vda5050::NodeState
OrderManager::node_to_state(const vda5050::Node& n)
{
  vda5050::NodeState ns;
  ns.node_id         = n.node_id;
  ns.sequence_id     = n.sequence_id;
  ns.node_description = n.node_description;
  ns.released        = n.released;
  ns.node_position   = n.node_position;
  return ns;
}

// Convert Edge (e) to EdgeState for State message publication.
vda5050::EdgeState
OrderManager::edge_to_state(const vda5050::Edge& e) {
  vda5050::EdgeState es;
  es.edge_id         = e.edge_id;
  es.sequence_id     = e.sequence_id;
  es.edge_description = e.edge_description;
  es.released        = e.released;
  es.trajectory      = e.trajectory;
  return es;
}

}  // namespace vda5050_client_adapter
