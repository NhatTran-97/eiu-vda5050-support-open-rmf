#include "vda5050_client_adapter/order_manager.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace vda5050_adapter {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

OrderManager::OrderManager() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Callback registration
// ─────────────────────────────────────────────────────────────────────────────

void OrderManager::set_order_accepted_callback(OrderAcceptedCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_order_accepted_ = std::move(cb);
}

void OrderManager::set_order_cancelled_callback(OrderCancelledCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_order_cancelled_ = std::move(cb);
}

void OrderManager::set_new_base_request_callback(NewBaseRequestCallback cb) 
{
  std::lock_guard<std::mutex> lock(mutex_);
  on_new_base_request_ = std::move(cb);
}

// ─────────────────────────────────────────────────────────────────────────────
// Process incoming order
// ─────────────────────────────────────────────────────────────────────────────

OrderAcceptResult OrderManager::process_order(const vda5050::Order& order) 
{
  std::unique_lock<std::mutex> lock(mutex_);

  OrderAcceptResult result;

  // ── Case 1: Completely new order ─────────────────────────────────────────
  if (order.order_id != current_order_id_) 
  {
    result = validate_new_order(order);
    if (!result.accepted) return result;

    apply_order(order);
    result.accepted = true;

    auto accepted_cb = on_order_accepted_;
    auto remaining_nodes = remaining_base_nodes_;
    auto remaining_edges = remaining_base_edges_;
    remaining_nodes.insert(remaining_nodes.end(),
                           horizon_nodes_.begin(), horizon_nodes_.end());
    remaining_edges.insert(remaining_edges.end(),
                           horizon_edges_.begin(), horizon_edges_.end());
    lock.unlock();

    if (accepted_cb) 
    {
      accepted_cb(order.order_id, order.order_update_id,
                  remaining_nodes, remaining_edges);
    }
    return result;
  }

  // ── Case 2: Order update (same orderId) ───────────────────────────────────
  if (order.order_update_id <= current_order_update_id_) 
  {
    result.accepted         = false;
    result.rejection_reason =
      "orderUpdateId must be greater than current (" +
      std::to_string(current_order_update_id_) + ")";
    return result;
  }

  result = validate_update(order);
  if (!result.accepted) return result;

  apply_stitch(order);
  result.accepted = true;

  auto accepted_cb = on_order_accepted_;
  auto remaining_nodes = remaining_base_nodes_;
  auto remaining_edges = remaining_base_edges_;
  remaining_nodes.insert(remaining_nodes.end(),
                         horizon_nodes_.begin(), horizon_nodes_.end());
  remaining_edges.insert(remaining_edges.end(),
                         horizon_edges_.begin(), horizon_edges_.end());
  lock.unlock();

  if (accepted_cb) {
    accepted_cb(order.order_id, order.order_update_id,
                remaining_nodes, remaining_edges);
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cancel
// ─────────────────────────────────────────────────────────────────────────────

void OrderManager::cancel_order(const std::string& order_id) 
{
  std::unique_lock<std::mutex> lock(mutex_);

  if (!order_id.empty() && order_id != current_order_id_) {
    std::cerr << "[OrderManager] cancel_order: id mismatch ('"
              << order_id << "' vs '" << current_order_id_ << "')\n";
    return;
  }

  std::string cancelled_id = current_order_id_;

  remaining_base_nodes_.clear();
  remaining_base_edges_.clear();
  horizon_nodes_.clear();
  horizon_edges_.clear();
  active_edges_.clear();
  current_order_id_         = "";
  current_order_update_id_  = 0;
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

bool OrderManager::node_reached(const NodeReachedEvent& evt) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!order_active_ || remaining_base_nodes_.empty()) {
    std::cerr << "[OrderManager] Unexpected node_reached: no released base node pending\n";
    return false;
  }

  const auto& expected_node = remaining_base_nodes_.front();
  if (expected_node.node_id != evt.node_id ||
      expected_node.sequence_id != evt.sequence_id) {
    std::cerr << "[OrderManager] Out-of-order node_reached: expected '"
              << expected_node.node_id << "' (seq=" << expected_node.sequence_id
              << "), got '" << evt.node_id << "' (seq=" << evt.sequence_id << ")\n";
    return false;
  }

  last_node_id_            = evt.node_id;
  last_node_sequence_id_   = evt.sequence_id;
  distance_since_last_node_ = evt.distance_driven;

  remaining_base_nodes_.erase(remaining_base_nodes_.begin());

  // Route fully consumed — order done.
  if (remaining_base_nodes_.empty() && horizon_nodes_.empty()) {
    order_active_ = false;
  }

  const bool should_request = (remaining_base_nodes_.size() < 2) &&
                              !horizon_nodes_.empty();
  const bool notify_new_base_request = should_request && !new_base_request_;
  new_base_request_ = should_request;

  auto new_base_cb =
    notify_new_base_request ? on_new_base_request_ : NewBaseRequestCallback{};

  lock.unlock();
  if (new_base_cb) new_base_cb();
  return true;
}

void OrderManager::edge_entered(const std::string& edge_id,
                                uint32_t           sequence_id)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = std::find_if(remaining_base_edges_.begin(), remaining_base_edges_.end(),
    [&](const vda5050::Edge& e) {
      return e.edge_id == edge_id && e.sequence_id == sequence_id;
    });

  if (it != remaining_base_edges_.end()) 
  {
    active_edges_.push_back(*it);
    remaining_base_edges_.erase(it);
  } else 
  {
    // Not in base — may be horizon edge or re-entry after stitch.
    std::cerr << "[OrderManager] edge_entered: edge '" << edge_id
              << "' (seq=" << sequence_id << ") not found in remaining base edges\n";
  }
}

bool OrderManager::edge_completed(const std::string& edge_id,
                                  uint32_t           sequence_id)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto active_it = std::find_if(active_edges_.begin(), active_edges_.end(),[&](const vda5050::Edge& e) {
      return e.edge_id == edge_id && e.sequence_id == sequence_id;});

  if (active_it != active_edges_.end()) 
  {
    active_edges_.erase(active_it);
    // Also clean up from base if edge_entered was skipped.
    auto base_it = std::find_if(remaining_base_edges_.begin(), remaining_base_edges_.end(),
      [&](const vda5050::Edge& e) 
      {
        return e.edge_id == edge_id && e.sequence_id == sequence_id;
      });
    if (base_it != remaining_base_edges_.end()) 
    {
      remaining_base_edges_.erase(base_it);
    }
    return true;
  }

  // Fallback: edge_entered was skipped, consume from base directly.
  if (!order_active_ || remaining_base_edges_.empty())
  {
    std::cerr << "[OrderManager] Unexpected edge_completed: no released base edge pending\n";
    return false;
  }

  const auto& expected_edge = remaining_base_edges_.front();
  if (expected_edge.edge_id != edge_id || expected_edge.sequence_id != sequence_id)
  {
    std::cerr << "[OrderManager] Out-of-order edge_completed: expected '"
              << expected_edge.edge_id << "' (seq=" << expected_edge.sequence_id
              << "), got '" << edge_id << "' (seq=" << sequence_id << ")\n";
    return false;
  }

  remaining_base_edges_.erase(remaining_base_edges_.begin());
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// State queries
// ─────────────────────────────────────────────────────────────────────────────

std::string OrderManager::current_order_id() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return current_order_id_;
}

uint32_t OrderManager::current_order_update_id() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return current_order_update_id_;
}

std::string OrderManager::last_node_id() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return last_node_id_;
}

uint32_t OrderManager::last_node_sequence_id() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return last_node_sequence_id_;
}

std::string OrderManager::current_zone_set_id() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return current_zone_set_id_;
}

double OrderManager::distance_since_last_node() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return distance_since_last_node_;
}

std::vector<vda5050::NodeState> OrderManager::node_states() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::NodeState> result;
  result.reserve(remaining_base_nodes_.size() + horizon_nodes_.size());
  for (const auto& n : remaining_base_nodes_) result.push_back(node_to_state(n));
  for (const auto& n : horizon_nodes_)        result.push_back(node_to_state(n));
  return result;
}

std::vector<vda5050::EdgeState> OrderManager::edge_states() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::EdgeState> result;
  result.reserve(active_edges_.size() + remaining_base_edges_.size() + horizon_edges_.size());
  for (const auto& e : active_edges_)         result.push_back(edge_to_state(e));
  for (const auto& e : remaining_base_edges_) result.push_back(edge_to_state(e));
  for (const auto& e : horizon_edges_)        result.push_back(edge_to_state(e));
  return result;
}

std::vector<vda5050::EdgeState> OrderManager::active_edge_states() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<vda5050::EdgeState> result;
  result.reserve(active_edges_.size());
  for (const auto& e : active_edges_) result.push_back(edge_to_state(e));
  return result;
}

bool OrderManager::new_base_request() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return new_base_request_;
}

bool OrderManager::has_active_order() const 
{
  std::lock_guard<std::mutex> lock(mutex_);
  return order_active_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

OrderAcceptResult
OrderManager::validate_new_order(const vda5050::Order& order) const 
{
  if (order.nodes.empty()) 
  {
    return {false, "Order must contain at least one node"};
  }

  if (order_active_) 
  {
    const bool route_remaining =
      !remaining_base_nodes_.empty() || !remaining_base_edges_.empty() ||
      !horizon_nodes_.empty() || !horizon_edges_.empty();
    if (route_remaining) 
    {
      return {false,
              "New order cannot replace the active order until the current ""route is fully consumed"};
    }
  }

  if (order_active_ && !last_node_id_.empty()) 
  {
    const auto& first_node = order.nodes.front();
    if (first_node.node_id != last_node_id_ ||
        first_node.sequence_id != last_node_sequence_id_) 
        {
      return {false,
              "New order's first node must match last traversed node "
              "(id=" + last_node_id_ + " seq=" +
              std::to_string(last_node_sequence_id_) + ")"};
    }
  }
  return {true, ""};
}

OrderAcceptResult
OrderManager::validate_update(const vda5050::Order& update) const
{
  if (update.nodes.empty()) 
  {
    return {false, "Order update must contain at least one node"};
  }

  const auto& stitch_node = update.nodes.front();

  bool stitch_ok = false;

  if (!horizon_nodes_.empty()) 
  {
    const auto& last_horizon = horizon_nodes_.back();
    stitch_ok = (stitch_node.node_id    == last_horizon.node_id &&
                 stitch_node.sequence_id == last_horizon.sequence_id);
  }

  if (!stitch_ok && !remaining_base_nodes_.empty()) 
  {
    const auto& last_base = remaining_base_nodes_.back();
    stitch_ok = (stitch_node.node_id    == last_base.node_id &&
                 stitch_node.sequence_id == last_base.sequence_id);
  }

  if (!stitch_ok && remaining_base_nodes_.empty() && horizon_nodes_.empty()) 
  {
    stitch_ok = (stitch_node.node_id    == last_node_id_ &&
                 stitch_node.sequence_id == last_node_sequence_id_);
  }

  if (!stitch_ok) {
    const std::string expected_id =
      !horizon_nodes_.empty()        ? horizon_nodes_.back().node_id :
      !remaining_base_nodes_.empty() ? remaining_base_nodes_.back().node_id :
                                       last_node_id_;
    return {false, "Order update stitch node mismatch: expected node_id=" + expected_id};
  }
  return {true, ""};
}

void OrderManager::apply_order(const vda5050::Order& order) 
{
  current_order_id_        = order.order_id;
  current_order_update_id_ = order.order_update_id;
  current_zone_set_id_     = order.zone_set_id;
  order_active_            = true;
  new_base_request_        = false;
  distance_since_last_node_ = 0.0;

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

void OrderManager::apply_stitch(const vda5050::Order& update) {
  current_order_update_id_ = update.order_update_id;
  new_base_request_        = false;

  horizon_nodes_.clear();
  horizon_edges_.clear();


  const bool stitch_node_is_remaining_base =
    !remaining_base_nodes_.empty() &&
    remaining_base_nodes_.back().node_id == update.nodes.front().node_id &&
    remaining_base_nodes_.back().sequence_id == update.nodes.front().sequence_id;

  const bool stitch_node_is_last_traversed =
    remaining_base_nodes_.empty() &&
    last_node_id_ == update.nodes.front().node_id &&
    last_node_sequence_id_ == update.nodes.front().sequence_id;

  const size_t first_new_node_index =
    (stitch_node_is_remaining_base || stitch_node_is_last_traversed) ? 1u : 0u;

  for (size_t i = first_new_node_index; i < update.nodes.size(); ++i) 
  {
    const auto& n = update.nodes[i];
    if (n.released)
    {
      remaining_base_nodes_.push_back(n);
    } 
    else 
    {
      horizon_nodes_.push_back(n);

    }           
  }
  for (const auto& e : update.edges) 
  {
    if (e.released) remaining_base_edges_.push_back(e);
    else  
    {
      horizon_edges_.push_back(e);
    }          
    
  }
}

// ─── Static converters ────────────────────────────────────────────────────────

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
