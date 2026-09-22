#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vda5050_msgs/msg/edge.hpp>
#include <vda5050_msgs/msg/edge_state.hpp>
#include <vda5050_msgs/msg/node.hpp>
#include <vda5050_msgs/msg/node_state.hpp>
#include <vda5050_msgs/msg/order.hpp>

namespace tb3_vda5050_bridge {

struct TraversalEvent {
  std::optional<vda5050_msgs::msg::EdgeState> edge_entered;
  std::optional<vda5050_msgs::msg::EdgeState> edge_completed;
  std::optional<vda5050_msgs::msg::NodeState> node_reached;
};

struct NavigationTarget {
  std::size_t node_index{0};
  vda5050_msgs::msg::Node node;
  std::optional<vda5050_msgs::msg::EdgeState> incoming_edge;
  // Speed limit from the incoming edge; -1 = not set.
  double incoming_edge_max_speed{-1.0};
};

enum class DispatchKind {
  NAVIGATE,
  WAITING_FOR_RELEASE,
  COMPLETED
};

struct DispatchPlan {
  DispatchKind kind{DispatchKind::COMPLETED};
  std::vector<TraversalEvent> immediate_events;
  std::optional<NavigationTarget> target;
};

// Tracks the order cursor, merges updates and plans the next route step.
class OrderSession {
public:
  // Starts an order, resuming from `resume_cursor`.
  void start(const vda5050_msgs::msg::Order& order, std::size_t resume_cursor = 0);

  // Merges an update of the current order; false if its update id is not newer.
  bool update(const vda5050_msgs::msg::Order& order);

  // Ends the order and resets the cursor.
  void clear();

  // Whether an order is active.
  bool has_order() const;

  const std::string& order_id() const { return current_order_id_; }

  // Index of the next node to process.
  std::size_t current_node_index() const { return current_node_index_; }

  // Counter bumped on start and clear, used to drop stale callbacks.
  uint64_t generation() const { return generation_; }

  const std::vector<vda5050_msgs::msg::Node>& nodes() const { return current_order_.nodes; }

  // Plans the next step: emit events, navigate, or wait for release.
  DispatchPlan plan_next_work();

  // Marks the node at `node_index` reached; empty if it is not the cursor node.
  std::vector<TraversalEvent> complete_navigation(std::size_t node_index);

  // Whether the next node is released but has no position, so only the robot pose can confirm it.
  bool next_node_requires_pose_to_complete() const;

private:
  // NodeState of `node`.
  static vda5050_msgs::msg::NodeState make_node_state(const vda5050_msgs::msg::Node& node);
  // Edge of `order` that ends at `node`.
  static std::optional<vda5050_msgs::msg::Edge> find_incoming_edge(
    const vda5050_msgs::msg::Order& order,
    const vda5050_msgs::msg::Node& node);
  // EdgeState of the edge that ends at `node`, for edge_entered and edge_completed events.
  static std::optional<vda5050_msgs::msg::EdgeState> make_incoming_edge_state(
    const vda5050_msgs::msg::Order& order,
    const vda5050_msgs::msg::Node& node);

  vda5050_msgs::msg::Order current_order_;
  std::string current_order_id_;
  std::size_t current_node_index_{0};
  uint64_t generation_{0};
};

}  // namespace tb3_vda5050_bridge
