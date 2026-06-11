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

class OrderSession {
public:
  void start(const vda5050_msgs::msg::Order& order);
  void update(const vda5050_msgs::msg::Order& order);
  void clear();

  bool has_order() const;
  const std::string& order_id() const { return current_order_id_; }
  std::size_t current_node_index() const { return current_node_index_; }
  uint64_t generation() const { return generation_; }

  DispatchPlan plan_next_work();
  std::vector<TraversalEvent> complete_navigation(std::size_t node_index);

private:
  static vda5050_msgs::msg::NodeState make_node_state(const vda5050_msgs::msg::Node& node);
  static std::optional<vda5050_msgs::msg::EdgeState> make_incoming_edge_state(
    const vda5050_msgs::msg::Order& order,
    const vda5050_msgs::msg::Node& node);

  vda5050_msgs::msg::Order current_order_;
  std::string current_order_id_;
  std::size_t current_node_index_{0};
  uint64_t generation_{0};
};

}  // namespace tb3_vda5050_bridge
