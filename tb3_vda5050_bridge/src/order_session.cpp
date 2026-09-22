#include "tb3_vda5050_bridge/order_session.hpp"

#include <algorithm>

namespace tb3_vda5050_bridge {

namespace {

// Merges `incoming` into `current` by sequence id: upserts entries, drops missing unreleased ones, keeps traversed ones.
template <typename T>
void merge_by_sequence_id(
  std::vector<T>& current, const std::vector<T>& incoming,
  std::optional<uint32_t> protect_below_sequence_id) {
  std::vector<T> merged;
  merged.reserve(current.size() + incoming.size());
  for (const auto& existing : current) {
    const bool still_present = std::any_of(
      incoming.begin(), incoming.end(), [&](const T& item) { return item.sequence_id == existing.sequence_id; });
    if (existing.released || still_present) {
      merged.push_back(existing);
    }
  }
  for (const auto& item : incoming) {
    auto it = std::find_if(merged.begin(), merged.end(),
      [&](const T& e) { return e.sequence_id == item.sequence_id; });
    if (it == merged.end()) {
      merged.push_back(item);
    } else if (!protect_below_sequence_id.has_value() ||
               it->sequence_id > *protect_below_sequence_id) {
      *it = item;
    }
  }
  std::sort(merged.begin(), merged.end(),
    [](const T& a, const T& b) { return a.sequence_id < b.sequence_id; });
  current = std::move(merged);
}

}  // namespace

// Stores the order, sets the cursor (resume point or 0) and bumps the generation.
void OrderSession::start(const vda5050_msgs::msg::Order& order, std::size_t resume_cursor) {
  current_order_ = order;
  current_order_id_ = order.order_id;
  current_node_index_ = std::min(resume_cursor, current_order_.nodes.size());
  ++generation_;
}

// Merges a newer update without changing traversed nodes.
bool OrderSession::update(const vda5050_msgs::msg::Order& order) {
  if (order.order_update_id <= current_order_.order_update_id) {
    return false;
  }
  std::optional<uint32_t> protect_below_sequence_id;
  if (current_node_index_ > 0 && current_node_index_ <= current_order_.nodes.size()) {
    protect_below_sequence_id = current_order_.nodes[current_node_index_ - 1].sequence_id;
  }
  merge_by_sequence_id(current_order_.nodes, order.nodes, protect_below_sequence_id);
  merge_by_sequence_id(current_order_.edges, order.edges, protect_below_sequence_id);
  current_order_.header = order.header;
  current_order_.order_update_id = order.order_update_id;
  return true;
}

// Clears the order and cursor and bumps the generation.
void OrderSession::clear() {
  current_order_ = vda5050_msgs::msg::Order{};
  current_order_id_.clear();
  current_node_index_ = 0;
  ++generation_;
}

// Whether an order is active.
bool OrderSession::has_order() const {
  return !current_order_id_.empty();
}

// Emits events for position-less nodes, then returns the next step: navigate, wait or completed.
DispatchPlan OrderSession::plan_next_work() {
  DispatchPlan plan;

  if (!has_order()) {
    plan.kind = DispatchKind::COMPLETED;
    return plan;
  }

  while (current_node_index_ < current_order_.nodes.size()) 
  {
    const auto& node = current_order_.nodes[current_node_index_];

    if (!node.released) {
      plan.kind = DispatchKind::WAITING_FOR_RELEASE;
      return plan;
    }

    if (!node.node_position_set) {
      TraversalEvent event;
      event.edge_entered = make_incoming_edge_state(current_order_, node);
      event.edge_completed = make_incoming_edge_state(current_order_, node);
      event.node_reached = make_node_state(node);
      plan.immediate_events.push_back(std::move(event));
      ++current_node_index_;
      continue;
    }

    NavigationTarget target;
    target.node_index = current_node_index_;
    target.node = node;
    const auto edge = find_incoming_edge(current_order_, node);
    if (edge.has_value()) {
      target.incoming_edge_max_speed = edge->max_speed;
    }
    target.incoming_edge = make_incoming_edge_state(current_order_, node);

    plan.kind = DispatchKind::NAVIGATE;
    plan.target = std::move(target);
    return plan;
  }

  plan.kind = DispatchKind::COMPLETED;
  return plan;
}

// Whether the cursor node is released but has no position, so the robot pose must confirm it.
bool OrderSession::next_node_requires_pose_to_complete() const {
  if (!has_order() || current_node_index_ >= current_order_.nodes.size()) {
    return false;
  }
  const auto& node = current_order_.nodes[current_node_index_];
  return node.released && !node.node_position_set;
}

// Advances the cursor and returns the traversal events of the reached node.
std::vector<TraversalEvent> OrderSession::complete_navigation(std::size_t node_index) {
  std::vector<TraversalEvent> events;

  if (!has_order() || node_index != current_node_index_ ||  node_index >= current_order_.nodes.size()) {
    return events;
  }

  const auto& node = current_order_.nodes[node_index];
  if (!node.released || !node.node_position_set) {
    return events;
  }

  TraversalEvent event;
  event.edge_completed = make_incoming_edge_state(current_order_, node);
  event.node_reached = make_node_state(node);
  events.push_back(std::move(event));

  current_node_index_ = node_index + 1;
  return events;
}

// NodeState of `node`.
vda5050_msgs::msg::NodeState OrderSession::make_node_state(const vda5050_msgs::msg::Node& node) {
  vda5050_msgs::msg::NodeState state;
  state.node_id = node.node_id;
  state.sequence_id = node.sequence_id;
  state.node_description = node.node_description;
  state.released = node.released;
  if (node.node_position_set) 
  {
    state.node_position = node.node_position;
    state.node_position_set = true;
  }
  return state;
}

// Edge of the order that ends at `node`, by sequence id.
std::optional<vda5050_msgs::msg::Edge> OrderSession::find_incoming_edge(
  const vda5050_msgs::msg::Order& order,
  const vda5050_msgs::msg::Node& node)
{
  if (node.sequence_id == 0) {
    return std::nullopt;
  }

  const auto edge_seq = node.sequence_id - 1;
  const auto it = std::find_if(order.edges.begin(), order.edges.end(), [edge_seq](const vda5050_msgs::msg::Edge& edge) 
  {
      return edge.sequence_id == edge_seq;
    });

  if (it == order.edges.end()) {
    return std::nullopt;
  }
  return *it;
}

// EdgeState of the edge that ends at `node`; nullopt if there is none.
std::optional<vda5050_msgs::msg::EdgeState> OrderSession::make_incoming_edge_state(const vda5050_msgs::msg::Order& order, const vda5050_msgs::msg::Node& node)
{
  const auto edge = find_incoming_edge(order, node);
  if (!edge.has_value()) 
  {
    return std::nullopt;
  }

  vda5050_msgs::msg::EdgeState state;
  state.edge_id = edge->edge_id;
  state.sequence_id = edge->sequence_id;
  state.edge_description = edge->edge_description;
  state.released = edge->released;
  if (edge->trajectory_set) {
    state.trajectory = edge->trajectory;
    state.trajectory_set = true;
  }
  return state;
}

}  // namespace tb3_vda5050_bridge
