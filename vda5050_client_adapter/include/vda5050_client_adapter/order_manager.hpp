#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "vda5050_client_adapter/vda5050_types.hpp"

namespace vda5050_adapter {

/**
 * @brief Result of VDA5050 order-acceptance validation.
 */
struct OrderAcceptResult
{
  bool        accepted{false};           ///< true if order passed validation and was accepted.
  std::string rejection_reason;          ///< Reason if accepted=false.
};

/// Data passed with a node-reached event.
struct NodeReachedEvent
{
  std::string node_id;
  uint32_t    sequence_id;
  double      distance_driven;  ///< meters since previous node
};

/**
 * @brief Manages VDA5050 order state: validation, stitching, base/horizon tracking, newBaseRequest.
 *
 * Validates incoming orders, manages order updates, tracks base (released, to-be-driven) and
 * horizon (unreleased, may change) segments, and signals when to request new base segments.
 * Handles stitch validation (update continuity), rejects stale orders, and fires callbacks
 * on state changes. Thread-safe via std::mutex.
 *
 * Key responsibilities:
 *  - Validate order acceptance per VDA5050 spec and internal invariants
 *  - Stitch order updates while protecting already-traversed nodes
 *  - Track base/horizon and emit newBaseRequest when base depletes below threshold
 *  - Report navigation events (node_reached, edge_entered, edge_completed)
 *  - Fire callbacks on order acceptance/cancellation and base request signals
 */
class OrderManager
{
public:
  // ─── Callbacks ────────────────────────────────────────────────────────────

  /**
   * @brief Callback when a new or updated order is accepted.
   * @param order_id Order ID.
   * @param order_update_id Order update ID.
   * @param remaining_nodes Remaining nodes (base + horizon).
   * @param remaining_edges Remaining edges (base + horizon).
   */
  using OrderAcceptedCallback =
    std::function<void(const std::string& order_id,
                       uint32_t           order_update_id,
                       const std::vector<vda5050::Node>& remaining_nodes,
                       const std::vector<vda5050::Edge>& remaining_edges)>;

  /**
   * @brief Callback when a cancelOrder instantAction is received.
   * @param order_id ID of the cancelled order.
   */
  using OrderCancelledCallback =
    std::function<void(const std::string& order_id)>;

  /**
   * @brief Callback to signal that a new base segment should be requested.
   */
  using NewBaseRequestCallback = std::function<void()>;

  /**
   * @brief Construct an order manager with no active order.
   */
  OrderManager();

  /**
   * @brief Destructor.
   */
  ~OrderManager() = default;

  // ─── Order ingestion ──────────────────────────────────────────────────────

  /**
   * @brief Validate and apply an incoming order.
   * @param order The VDA5050 order to process.
   * @return OrderAcceptResult with acceptance status and optional rejection reason.
   */
  OrderAcceptResult process_order(const vda5050::Order& order);

  /**
   * @brief Cancel the active order.
   * @param order_id If non-empty, only cancel if it matches the current order ID.
   */
  void cancel_order(const std::string& order_id = "");

  // ─── Navigation feedback (called by the robot driver) ─────────────────────

  bool node_reached(const NodeReachedEvent& evt);
  bool edge_entered(const std::string& edge_id, uint32_t sequence_id);
  bool edge_completed(const std::string& edge_id, uint32_t sequence_id);

  // ─── State queries ────────────────────────────────────────────────────────

  std::string  current_order_id()        const;
  uint32_t     current_order_update_id() const;
  std::string  last_node_id()            const;
  uint32_t     last_node_sequence_id()   const;
  std::string  current_zone_set_id()     const;
  double       distance_since_last_node() const;

  /// Returns a snapshot of all remaining node states (base + horizon).
  std::vector<vda5050::NodeState> node_states() const;

  /// Returns a snapshot of all remaining edge states (base + horizon), including active edges.
  std::vector<vda5050::EdgeState> edge_states() const;

  /// Returns a snapshot of currently active (entered but not yet completed) edges.
  std::vector<vda5050::EdgeState> active_edge_states() const;

  /// True if the AGV should request a new base segment.
  bool new_base_request() const;

  /// True if there is an active order in progress.
  bool has_active_order() const;

  // ─── Callbacks registration ───────────────────────────────────────────────

  void set_order_accepted_callback(OrderAcceptedCallback cb);
  void set_order_cancelled_callback(OrderCancelledCallback cb);
  void set_new_base_request_callback(NewBaseRequestCallback cb);

private:
  // ─── Internal helpers ─────────────────────────────────────────────────────

  OrderAcceptResult validate_new_order(const vda5050::Order& order) const;
  OrderAcceptResult validate_update(const vda5050::Order& order) const;

  void apply_order(const vda5050::Order& order);
  void apply_stitch(const vda5050::Order& update);

  static vda5050::NodeState node_to_state(const vda5050::Node& n);
  static vda5050::EdgeState edge_to_state(const vda5050::Edge& e);

  // ─── State ────────────────────────────────────────────────────────────────

  mutable std::mutex mutex_;

  std::string current_order_id_;
  uint32_t    current_order_update_id_{0};
  std::string current_zone_set_id_;

  std::string last_node_id_;
  uint32_t    last_node_sequence_id_{0};
  double      distance_since_last_node_{0.0};

  std::vector<vda5050::Node>  remaining_base_nodes_;
  std::vector<vda5050::Edge>  remaining_base_edges_;
  std::vector<vda5050::Node>  horizon_nodes_;
  std::vector<vda5050::Edge>  horizon_edges_;
  std::vector<vda5050::Edge>  active_edges_;  ///< Edges currently being traversed (entered but not completed)

  bool new_base_request_{false};
  bool order_active_{false};

  // ─── Callbacks ────────────────────────────────────────────────────────────

  OrderAcceptedCallback   on_order_accepted_;
  OrderCancelledCallback  on_order_cancelled_;
  NewBaseRequestCallback  on_new_base_request_;
};

}  // namespace vda5050_adapter
