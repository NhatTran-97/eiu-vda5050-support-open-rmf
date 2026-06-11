#include "tb3_vda5050_bridge/order_session.hpp"

#include <algorithm>

namespace tb3_vda5050_bridge {

void OrderSession::start(const vda5050_msgs::msg::Order& order) {
  current_order_ = order;
  current_order_id_ = order.order_id;
  current_node_index_ = 0;
  ++generation_;
}

void OrderSession::update(const vda5050_msgs::msg::Order& order) {

  for (const auto& node : order.nodes) {
    auto it = std::find_if(current_order_.nodes.begin(), current_order_.nodes.end(),
      [&](const vda5050_msgs::msg::Node& n) { return n.sequence_id == node.sequence_id; });
    if (it == current_order_.nodes.end()) {
      current_order_.nodes.push_back(node);
    } else {
      *it = node;
    }
  }
  for (const auto& edge : order.edges) {
    auto it = std::find_if(current_order_.edges.begin(), current_order_.edges.end(),
      [&](const vda5050_msgs::msg::Edge& e) { return e.sequence_id == edge.sequence_id; });
    if (it == current_order_.edges.end()) {
      current_order_.edges.push_back(edge);
    } else {
      *it = edge;
    }
  }
  current_order_.header = order.header;
  current_order_.order_update_id = order.order_update_id;
}

void OrderSession::clear() {
  current_order_ = vda5050_msgs::msg::Order{};
  current_order_id_.clear();
  current_node_index_ = 0;
  ++generation_;
}

bool OrderSession::has_order() const {
  return !current_order_id_.empty();
}

DispatchPlan OrderSession::plan_next_work() {
  DispatchPlan plan;

  if (!has_order()) {
    plan.kind = DispatchKind::COMPLETED;
    return plan;
  }

  while (current_node_index_ < current_order_.nodes.size()) {
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
    target.incoming_edge = make_incoming_edge_state(current_order_, node);

    plan.kind = DispatchKind::NAVIGATE;
    plan.target = std::move(target);
    return plan;
  }

  plan.kind = DispatchKind::COMPLETED;
  return plan;
}

std::vector<TraversalEvent> OrderSession::complete_navigation(std::size_t node_index) {
  std::vector<TraversalEvent> events;

  if (!has_order() || node_index != current_node_index_ ||
      node_index >= current_order_.nodes.size()) {
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

vda5050_msgs::msg::NodeState OrderSession::make_node_state(const vda5050_msgs::msg::Node& node) {
  vda5050_msgs::msg::NodeState state;
  state.node_id = node.node_id;
  state.sequence_id = node.sequence_id;
  state.node_description = node.node_description;
  state.released = node.released;
  if (node.node_position_set) {
    state.node_position = node.node_position;
    state.node_position_set = true;
  }
  return state;
}

std::optional<vda5050_msgs::msg::EdgeState> OrderSession::make_incoming_edge_state(
  const vda5050_msgs::msg::Order& order,
  const vda5050_msgs::msg::Node& node)
{
  if (node.sequence_id == 0) {
    return std::nullopt;
  }

  const auto edge_seq = node.sequence_id - 1;
  const auto it = std::find_if(order.edges.begin(), order.edges.end(),
    [edge_seq](const vda5050_msgs::msg::Edge& edge) {
      return edge.sequence_id == edge_seq;
    });

  if (it == order.edges.end()) {
    return std::nullopt;
  }

  vda5050_msgs::msg::EdgeState state;
  state.edge_id = it->edge_id;
  state.sequence_id = it->sequence_id;
  state.edge_description = it->edge_description;
  state.released = it->released;
  if (it->trajectory_set) {
    state.trajectory = it->trajectory;
    state.trajectory_set = true;
  }
  return state;
}

}  // namespace tb3_vda5050_bridge
