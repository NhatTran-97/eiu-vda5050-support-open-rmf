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

/**
 * @brief Manages VDA5050 order state: traversal cursor, node/edge sequencing, and plan generation.
 *
 * Stores and updates the active order, tracks progress through nodes/edges, and generates
 * dispatch plans (navigate, wait, or complete). Handles order updates by merging new content
 * while protecting already-traversed nodes and dropping orphaned horizon entries.
 *
 * Key responsibilities:
 *  - Store order state and cursor position across dispatches
 *  - Validate and merge order updates by sequence_id
 *  - Generate dispatch plans: navigate to next node, wait for release, or complete
 *  - Track generation counter to invalidate stale callbacks
 */
class OrderSession {
public:
  /**
   * @brief Start a new order or resume from a prior checkpoint.
   * @param order The VDA5050 order to start.
   * @param resume_cursor Node index to resume from (0 = start at beginning).
   */
  void start(const vda5050_msgs::msg::Order& order, std::size_t resume_cursor = 0);

  /**
   * @brief Merge an updated order into the current one.
   * @param order The updated order (must have order_id matching current).
   * @return false if order_update_id is not strictly newer (stale/duplicate), true on success.
   */
  bool update(const vda5050_msgs::msg::Order& order);

  /**
   * @brief Clear the active order session and reset state.
   */
  void clear();

  // ── State queries ──────────────────────────────────────────────────────

  /**
   * @brief Check if an order is currently active.
   * @return true if order_id is non-empty.
   */
  bool has_order() const;

  /**
   * @brief Get the current order ID.
   * @return Order ID string, empty if no active order.
   */
  const std::string& order_id() const { return current_order_id_; }

  /**
   * @brief Get the index of the next node to process.
   * @return Cursor position in nodes vector.
   */
  std::size_t current_node_index() const { return current_node_index_; }

  /**
   * @brief Get the generation counter (incremented on start/clear).
   * @return Generation number used to invalidate stale callbacks.
   */
  uint64_t generation() const { return generation_; }

  /**
   * @brief Get the current order's nodes.
   * @return Const reference to nodes vector.
   */
  const std::vector<vda5050_msgs::msg::Node>& nodes() const { return current_order_.nodes; }

  // ── Dispatch planning ──────────────────────────────────────────────────

  /**
   * @brief Plan the next work: emit events, navigate, or wait.
   * @return DispatchPlan with kind (NAVIGATE/WAITING_FOR_RELEASE/COMPLETED) and optional target.
   */
  DispatchPlan plan_next_work();

  /**
   * @brief Complete navigation to a node after reaching it.
   * @param node_index Index of the reached node.
   * @return Traversal events (edge_completed, node_reached) if index matches cursor, empty otherwise.
   */
  std::vector<TraversalEvent> complete_navigation(std::size_t node_index);

  /**
   * @brief Check if next node is position-less and requires pose validation before auto-completion.
   * @return true if next node is released but has no position set.
   */
  bool next_node_requires_pose_to_complete() const;

private:
  // Convert VDA5050 Node (node) to NodeState for publication.
  static vda5050_msgs::msg::NodeState make_node_state(const vda5050_msgs::msg::Node& node);
  // Find edge in order (order) preceding node (node) by sequence_id; return nullopt if not found.
  static std::optional<vda5050_msgs::msg::Edge> find_incoming_edge(
    const vda5050_msgs::msg::Order& order,
    const vda5050_msgs::msg::Node& node);
  // Build EdgeState (from order's edge) for node (node); used to publish edge_entered/edge_completed events.
  static std::optional<vda5050_msgs::msg::EdgeState> make_incoming_edge_state(
    const vda5050_msgs::msg::Order& order,
    const vda5050_msgs::msg::Node& node);

  vda5050_msgs::msg::Order current_order_;
  std::string current_order_id_;
  std::size_t current_node_index_{0};
  uint64_t generation_{0};
};

}  // namespace tb3_vda5050_bridge
