/**
 * @file test_order_manager.cpp
 * @brief Unit tests for vda5050_adapter::OrderManager.
 *
 * Coverage:
 *  - New order acceptance (base + horizon split)
 *  - Order update / stitching (from base node, from last-traversed)
 *  - Stale / invalid update rejection
 *  - node_reached / edge_completed feedback
 *  - new_base_request trigger logic
 *  - distance_since_last_node tracking
 *  - cancel_order (matching, empty, mismatched ID)
 *  - Replacement order (allowed regardless of remaining route; blocked only
 *    if it doesn't start from the robot's actual last-traversed node)
 *  - State-query consistency after mutations
 */

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vda5050_client_adapter/order_manager.hpp"

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────────

vda5050::Action make_action(const std::string& id,
                            const std::string& type = "noop",
                            vda5050::BlockingType bt = vda5050::BlockingType::NONE) {
  vda5050::Action a;
  a.action_id    = id;
  a.action_type  = type;
  a.blocking_type = bt;
  return a;
}

vda5050::Node make_node(const std::string& id, uint32_t seq, bool released,
                        std::vector<vda5050::Action> actions = {}) {
  vda5050::Node n;
  n.node_id     = id;
  n.sequence_id = seq;
  n.released    = released;
  n.actions     = std::move(actions);
  return n;
}

vda5050::Edge make_edge(const std::string& id, uint32_t seq, bool released,
                        const std::string& from, const std::string& to,
                        std::vector<vda5050::Action> actions = {}) {
  vda5050::Edge e;
  e.edge_id       = id;
  e.sequence_id   = seq;
  e.released      = released;
  e.start_node_id = from;
  e.end_node_id   = to;
  e.actions       = std::move(actions);
  return e;
}

vda5050::Order make_order(const std::string& oid, uint32_t uid,
                          std::vector<vda5050::Node> nodes,
                          std::vector<vda5050::Edge> edges) {
  vda5050::Order o;
  o.order_id        = oid;
  o.order_update_id = uid;
  o.nodes           = std::move(nodes);
  o.edges           = std::move(edges);
  return o;
}

vda5050_adapter::NodeReachedEvent evt(const std::string& nid,
                                      uint32_t           seq,
                                      double             dist = 0.0) {
  return {nid, seq, dist};
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// New order acceptance
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, AcceptsNewOrderAndFiresCallbackWithFullGraph) {
  vda5050_adapter::OrderManager mgr;

  int  cb_count   = 0;
  std::string cb_oid;
  uint32_t    cb_uid = 0;
  std::vector<vda5050::Node> cb_nodes;
  std::vector<vda5050::Edge> cb_edges;

  mgr.set_order_accepted_callback(
    [&](const std::string& oid, uint32_t uid,
        const std::vector<vda5050::Node>& ns,
        const std::vector<vda5050::Edge>& es) {
      ++cb_count; cb_oid = oid; cb_uid = uid; cb_nodes = ns; cb_edges = es;
    });

  const auto order = make_order("ord-1", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, false)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, false, "n2", "n3")});

  ASSERT_TRUE(mgr.process_order(order).accepted);

  EXPECT_EQ(cb_count, 1);
  EXPECT_EQ(cb_oid,  "ord-1");
  EXPECT_EQ(cb_uid,  1u);
  // callback receives base + horizon combined
  ASSERT_EQ(cb_nodes.size(), 3u);
  ASSERT_EQ(cb_edges.size(), 2u);

  EXPECT_EQ(mgr.current_order_id(),        "ord-1");
  EXPECT_EQ(mgr.current_order_update_id(), 1u);
  // lastNodeId is only updated when the robot physically reaches a node (node_reached()),
  // not at order acceptance time — so it remains empty until the first node is traversed.
  EXPECT_EQ(mgr.last_node_id(),            "");
  EXPECT_EQ(mgr.last_node_sequence_id(),   0u);
  EXPECT_TRUE(mgr.has_active_order());
}

TEST(OrderManagerTest, SplitsNodesAndEdgesIntoBaseAndHorizon) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, false)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, false, "n2", "n3")}
  )).accepted);

  const auto ns = mgr.node_states();
  const auto es = mgr.edge_states();

  ASSERT_EQ(ns.size(), 3u);
  ASSERT_EQ(es.size(), 2u);
  EXPECT_TRUE(ns[0].released);
  EXPECT_TRUE(ns[1].released);
  EXPECT_FALSE(ns[2].released);
  EXPECT_TRUE(es[0].released);
  EXPECT_FALSE(es[1].released);
}

TEST(OrderManagerTest, RejectsOrderWithNoNodes) {
  vda5050_adapter::OrderManager mgr;
  auto result = mgr.process_order(make_order("o", 1, {}, {}));
  EXPECT_FALSE(result.accepted);
}

TEST(OrderManagerTest, AllHorizonNodesOrderLastNodeIdRemainsEmptyUntilNodeReached) {
  // Edge case: order with all released=false nodes.
  // lastNodeId is only set when the robot physically arrives; at order acceptance it
  // stays at whatever it was before (empty for a fresh manager).
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, false), make_node("n2", 2, false)}, {}
  )).accepted);

  EXPECT_EQ(mgr.last_node_id(), "");
  EXPECT_EQ(mgr.last_node_sequence_id(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// node_reached feedback
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, NodeReachedAdvancesPointerAndTracksDistance) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  ASSERT_TRUE(mgr.node_reached(evt("n1", 0, 2.5)));

  EXPECT_EQ(mgr.last_node_id(),          "n1");
  EXPECT_EQ(mgr.last_node_sequence_id(), 0u);
  EXPECT_DOUBLE_EQ(mgr.distance_since_last_node(), 2.5);
  EXPECT_EQ(mgr.node_states().size(), 1u); // n1 consumed, n2 remains
}

TEST(OrderManagerTest, SetDistanceSinceLastNodeStreamsLiveProgress) {
  vda5050_adapter::OrderManager mgr;

  mgr.set_distance_since_last_node(1.2);
  EXPECT_DOUBLE_EQ(mgr.distance_since_last_node(), 1.2);

  // node_reached still wins as the exact value at arrival.
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);
  mgr.set_distance_since_last_node(3.7);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0, 2.5)));
  EXPECT_DOUBLE_EQ(mgr.distance_since_last_node(), 2.5);
}

TEST(OrderManagerTest, NodeReachedRejectsWrongIdWithoutMutatingState) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  EXPECT_FALSE(mgr.node_reached(evt("wrong", 0)));
  EXPECT_EQ(mgr.last_node_id(), "");         // still empty — no node has been reached
  EXPECT_EQ(mgr.node_states().size(), 2u);   // unchanged
}

TEST(OrderManagerTest, NodeReachedRejectsWrongSequenceId) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)}, {}
  )).accepted);

  EXPECT_FALSE(mgr.node_reached(evt("n1", 99)));  // wrong sequence
  EXPECT_EQ(mgr.node_states().size(), 2u);
}

TEST(OrderManagerTest, NodeReachedOnEmptyQueueReturnsFalse) {
  vda5050_adapter::OrderManager mgr;
  // No active order
  EXPECT_FALSE(mgr.node_reached(evt("n1", 0)));
}

// ─────────────────────────────────────────────────────────────────────────────
// edge_completed feedback
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, EdgeCompletedConsumesEdgeFromBase) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  ASSERT_TRUE(mgr.edge_completed("e12", 1));
  EXPECT_EQ(mgr.edge_states().size(), 0u);
}

TEST(OrderManagerTest, EdgeCompletedRejectsMismatchedEdge) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  EXPECT_FALSE(mgr.edge_completed("wrong", 1));
  EXPECT_EQ(mgr.edge_states().size(), 1u);  // unchanged
}

TEST(OrderManagerTest, EdgeCompletedOnEmptyQueueReturnsFalse) {
  vda5050_adapter::OrderManager mgr;
  EXPECT_FALSE(mgr.edge_completed("e", 0));
}

// ─────────────────────────────────────────────────────────────────────────────
// new_base_request
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, NewBaseRequestFiredWhenFewerThanTwoBaseNodesRemain) {
  vda5050_adapter::OrderManager mgr;
  int requests = 0;
  mgr.set_new_base_request_callback([&]{ ++requests; });

  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, false)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, false, "n2", "n3")}
  )).accepted);

  // After reaching n1, only n2 remains in base → < 2 → fire
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));
  EXPECT_EQ(requests, 1);
  EXPECT_TRUE(mgr.new_base_request());
}

TEST(OrderManagerTest, NewBaseRequestNotFiredWhenHorizonIsEmpty) {
  vda5050_adapter::OrderManager mgr;
  int requests = 0;
  mgr.set_new_base_request_callback([&]{ ++requests; });

  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));
  EXPECT_EQ(requests, 0);  // no horizon → no request
  EXPECT_FALSE(mgr.new_base_request());
}

TEST(OrderManagerTest, NewBaseRequestNotFiredTwiceForSameCondition) {
  vda5050_adapter::OrderManager mgr;
  int requests = 0;
  mgr.set_new_base_request_callback([&]{ ++requests; });

  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, false)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, false, "n2", "n3")}
  )).accepted);

  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));
  EXPECT_EQ(requests, 1);

  // A second call with "wrong" node returns false but must NOT re-fire
  mgr.node_reached(evt("n1", 0));  // wrong (already consumed), no-op
  EXPECT_EQ(requests, 1);
}

TEST(OrderManagerTest, NewBaseRequestClearedAfterOrderUpdateArrives) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, false)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, false, "n2", "n3")}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));
  ASSERT_TRUE(mgr.new_base_request());

  // Deliver update — request should clear
  const auto update = make_order("o", 2,
    {make_node("n2", 2, true), make_node("n3", 4, true), make_node("n4", 6, false)},
    {make_edge("e23", 3, true, "n2", "n3"), make_edge("e34", 5, false, "n3", "n4")});
  ASSERT_TRUE(mgr.process_order(update).accepted);
  EXPECT_FALSE(mgr.new_base_request());
}

// ─────────────────────────────────────────────────────────────────────────────
// Order update / stitching
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, RejectsUpdateWithNonIncreasingUpdateId) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 5,
    {make_node("n1", 0, true)}, {}
  )).accepted);

  // Same ID
  auto r1 = mgr.process_order(make_order("o", 5, {make_node("n1", 0, true)}, {}));
  EXPECT_FALSE(r1.accepted);
  EXPECT_NE(r1.rejection_reason.find("greater"), std::string::npos);

  // Smaller ID
  auto r2 = mgr.process_order(make_order("o", 3, {make_node("n1", 0, true)}, {}));
  EXPECT_FALSE(r2.accepted);
}

TEST(OrderManagerTest, StitchFromLastTraversedNodeWhenBaseIsEmpty) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true)}, {}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0, 1.0)));
  EXPECT_EQ(mgr.node_states().size(), 0u);  // base consumed

  const auto update = make_order("o", 2,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")});
  ASSERT_TRUE(mgr.process_order(update).accepted);

  // n1 (stitch) must NOT be duplicated — only n2 should appear
  const auto ns = mgr.node_states();
  ASSERT_EQ(ns.size(), 1u);
  EXPECT_EQ(ns.front().node_id, "n2");
  EXPECT_EQ(mgr.current_order_update_id(), 2u);
}

TEST(OrderManagerTest, StitchFromLastRemainingBaseNode) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, false)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, false, "n2", "n3")}
  )).accepted);

  // Stitch from n3 which is the last base remaining (still not traversed)
  const auto update = make_order("o", 2,
    {make_node("n3", 4, true), make_node("n4", 6, true)},
    {make_edge("e34", 5, true, "n3", "n4")});
  ASSERT_TRUE(mgr.process_order(update).accepted);

  const auto ns = mgr.node_states();
  // n1, n2 (base, unconsumed) + n3 (stitch, not duplicated) + n4 (new)
  // Result: n1, n2, n4 because n3 was already in remaining_base
  EXPECT_TRUE(std::any_of(ns.begin(), ns.end(),
    [](const auto& n){ return n.node_id == "n4"; }));
  // n3 must appear at most once (no duplication from stitching)
  const auto n3_count = std::count_if(ns.begin(), ns.end(),
    [](const auto& n){ return n.node_id == "n3"; });
  EXPECT_LE(n3_count, 1);
}

TEST(OrderManagerTest, RejectsUpdateWithNonMatchingStitchNode) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  // Wrong stitch node
  const auto bad_update = make_order("o", 2,
    {make_node("wrong", 99, true), make_node("n3", 4, true)},
    {make_edge("e23", 3, true, "wrong", "n3")});
  EXPECT_FALSE(mgr.process_order(bad_update).accepted);
  EXPECT_EQ(mgr.current_order_update_id(), 1u);  // unchanged
}

// ─────────────────────────────────────────────────────────────────────────────
// cancel_order
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, CancelOrderFiresCallbackAndClearsAllState) {
  vda5050_adapter::OrderManager mgr;
  std::string cancelled_id;
  mgr.set_order_cancelled_callback([&](const std::string& id){ cancelled_id = id; });

  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  mgr.cancel_order("o");

  EXPECT_EQ(cancelled_id, "o");
  EXPECT_EQ(mgr.node_states().size(),  0u);
  EXPECT_EQ(mgr.edge_states().size(),  0u);
  EXPECT_FALSE(mgr.has_active_order());
  EXPECT_FALSE(mgr.new_base_request());
}

TEST(OrderManagerTest, CancelOrderClearsZoneSetId) {
  vda5050_adapter::OrderManager mgr;

  auto order = make_order("o", 1, {make_node("n1", 0, true)}, {});
  order.zone_set_id = "zone-42";
  ASSERT_TRUE(mgr.process_order(order).accepted);
  ASSERT_EQ(mgr.current_zone_set_id(), "zone-42");

  mgr.cancel_order("o");

  EXPECT_EQ(mgr.current_zone_set_id(), "");
}

TEST(OrderManagerTest, CancelWithEmptyIdCancelsCurrentOrder) {
  vda5050_adapter::OrderManager mgr;
  std::string cancelled_id;
  mgr.set_order_cancelled_callback([&](const std::string& id){ cancelled_id = id; });

  ASSERT_TRUE(mgr.process_order(make_order("ord-42", 1,
    {make_node("n1", 0, true)}, {}
  )).accepted);

  mgr.cancel_order("");  // empty = cancel whatever is active

  EXPECT_EQ(cancelled_id, "ord-42");
  EXPECT_EQ(mgr.node_states().size(), 0u);
}

TEST(OrderManagerTest, CancelWithMismatchedIdDoesNothing) {
  vda5050_adapter::OrderManager mgr;
  std::string cancelled_id;
  mgr.set_order_cancelled_callback([&](const std::string& id){ cancelled_id = id; });

  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 0, true)}, {}
  )).accepted);

  mgr.cancel_order("ord-99");  // wrong ID

  EXPECT_TRUE(cancelled_id.empty());
  EXPECT_EQ(mgr.node_states().size(), 1u);  // unchanged
  EXPECT_TRUE(mgr.has_active_order());
}

// ─────────────────────────────────────────────────────────────────────────────
// Replacement order
// ─────────────────────────────────────────────────────────────────────────────

// RMF issues one fresh order_id per leg rather than stitching a single long
// order, so a replacement while the old order still has route left is normal.
TEST(OrderManagerTest, AcceptsReplacementOrderEvenWithRouteRemaining) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  auto result = mgr.process_order(make_order("ord-2", 1,
    {make_node("n1", 0, true)}, {}));
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(mgr.current_order_id(), "ord-2");
}

TEST(OrderManagerTest, AcceptsReplacementOrderAfterFullConsumption) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 0, true)}, {}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));

  ASSERT_TRUE(mgr.process_order(make_order("ord-2", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  EXPECT_EQ(mgr.current_order_id(), "ord-2");
  EXPECT_EQ(mgr.node_states().size(), 2u);
}

// The one continuity rule that IS still enforced: a replacement order must
// start from the node the robot is actually standing on.
TEST(OrderManagerTest, RejectsReplacementOrderStartingFromWrongNode) {
  vda5050_adapter::OrderManager mgr;
  // Two nodes, only the first reached, so the order is still active (a
  // single-node order would complete on node_reached and leave nothing to enforce continuity against).
  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));

  auto result = mgr.process_order(make_order("ord-2", 1,
    {make_node("wrong_node", 0, true)}, {}));
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(mgr.current_order_id(), "ord-1");
}

// sequence_id is per-order (a new order's base always restarts at 0), so it
// must not be compared against the previous order's last_node_sequence_id --  only node_id identifies where the robot physically is.
TEST(OrderManagerTest, AcceptsReplacementFromRightNodeDespiteDifferentSequenceId) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));
  ASSERT_TRUE(mgr.node_reached(evt("n2", 2)));

  // ord-2 starts from n2, but n2 is seq=0 here (a new order's own base),
  // not seq=2 like it was in ord-1.
  auto result = mgr.process_order(make_order("ord-2", 1,
    {make_node("n2", 0, true)}, {}));
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(mgr.current_order_id(), "ord-2");
}

// A late edge_entered/node_reached for whatever ord-1 still had queued,
// arriving after ord-2 already replaced it, is absorbed instead of rejected -- and must leave ord-2's own progress untouched.
TEST(OrderManagerTest, AbsorbsStaleEchoFromJustReplacedOrder) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));

  ASSERT_TRUE(mgr.process_order(make_order("ord-2", 1,
    {make_node("n1", 0, true), make_node("n4", 2, true)},
    {make_edge("e14", 1, true, "n1", "n4")}
  )).accepted);

  EXPECT_TRUE(mgr.edge_entered("e12", 1));
  EXPECT_TRUE(mgr.node_reached(evt("n2", 2)));

  EXPECT_EQ(mgr.current_order_id(), "ord-2");
  EXPECT_EQ(mgr.last_node_id(), "n1");        // unaffected by the stale echo
  EXPECT_EQ(mgr.node_states().size(), 2u);    // ord-2's own nodes, untouched
  EXPECT_TRUE(mgr.active_edge_states().empty());
}

// A stale last_node_sequence_id from the previous order lets RMF's own `passed = lastNodeSequenceId / 2` progress check (robot_command_handle.cpp)
// overshoot a new, shorter order's node count, reading it as already complete -- see memory "rmf-jazzy-unresponsive-handle-replan-bug".
TEST(OrderManagerTest, NewOrderResetsLastNodeSequenceId) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("ord-1", 1,
    {make_node("n1", 8, true)}, {}
  )).accepted);
  ASSERT_TRUE(mgr.node_reached(evt("n1", 8)));
  EXPECT_EQ(mgr.last_node_sequence_id(), 8u);

  ASSERT_TRUE(mgr.process_order(make_order("ord-2", 1,
    {make_node("n2", 0, true), make_node("n3", 2, true)},
    {make_edge("e23", 1, true, "n2", "n3")}
  )).accepted);

  EXPECT_EQ(mgr.last_node_sequence_id(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Full traversal cycle
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, FullOrderCycleLeavesMgrReadyForNextOrder) {
  vda5050_adapter::OrderManager mgr;

  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  ASSERT_TRUE(mgr.node_reached(evt("n1", 0)));
  ASSERT_TRUE(mgr.edge_completed("e12", 1));
  ASSERT_TRUE(mgr.node_reached(evt("n2", 2)));

  EXPECT_EQ(mgr.node_states().size(), 0u);
  EXPECT_EQ(mgr.edge_states().size(), 0u);
  EXPECT_EQ(mgr.last_node_id(), "n2");
  // Order becomes inactive once all nodes are traversed (no remaining base or horizon nodes).
  // A new order or replacement order must be sent to resume.
  EXPECT_FALSE(mgr.has_active_order());
}

TEST(OrderManagerTest, ZoneSetIdPersistedAndReturned) {
  vda5050_adapter::OrderManager mgr;
  vda5050::Order o;
  o.order_id        = "o";
  o.order_update_id = 1;
  o.zone_set_id     = "zone-42";
  o.nodes.push_back(make_node("n1", 0, true));

  ASSERT_TRUE(mgr.process_order(o).accepted);
  EXPECT_EQ(mgr.current_zone_set_id(), "zone-42");
}

TEST(OrderManagerTest, IdleManagerReportsCorrectInitialState) {
  vda5050_adapter::OrderManager mgr;

  EXPECT_EQ(mgr.current_order_id(),        "");
  EXPECT_EQ(mgr.current_order_update_id(), 0u);
  EXPECT_EQ(mgr.last_node_id(),            "");
  EXPECT_EQ(mgr.last_node_sequence_id(),   0u);
  EXPECT_FALSE(mgr.has_active_order());
  EXPECT_FALSE(mgr.new_base_request());
  EXPECT_DOUBLE_EQ(mgr.distance_since_last_node(), 0.0);
  EXPECT_EQ(mgr.node_states().size(), 0u);
  EXPECT_EQ(mgr.edge_states().size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// edge_entered / edge_completed active edge tracking
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderManagerTest, EdgeEnteredMovesEdgeToActiveState) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  // Before entering: edge is in remaining base, not active
  EXPECT_EQ(mgr.active_edge_states().size(), 0u);
  EXPECT_EQ(mgr.edge_states().size(), 1u);

  mgr.edge_entered("e12", 1);

  // After entering: edge should be in active_edges, removed from remaining_base
  ASSERT_EQ(mgr.active_edge_states().size(), 1u);
  EXPECT_EQ(mgr.active_edge_states().front().edge_id, "e12");

  // edge_states() must still report it (active edges are included)
  EXPECT_EQ(mgr.edge_states().size(), 1u);
}

TEST(OrderManagerTest, EdgeEnteredRejectsAnEdgeThatIsNotNextInSequence) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true), make_node("n3", 4, true)},
    {make_edge("e12", 1, true, "n1", "n2"), make_edge("e23", 3, true, "n2", "n3")}
  )).accepted);

  // e23 is not next — e12 is still the front of the remaining base edges.
  EXPECT_FALSE(mgr.edge_entered("e23", 3));
  EXPECT_EQ(mgr.active_edge_states().size(), 0u);

  // The correct next edge still works afterwards.
  EXPECT_TRUE(mgr.edge_entered("e12", 1));
  ASSERT_EQ(mgr.active_edge_states().size(), 1u);
  EXPECT_EQ(mgr.active_edge_states().front().edge_id, "e12");
}

TEST(OrderManagerTest, EdgeCompletedRemovesEdgeFromActiveState) {
  vda5050_adapter::OrderManager mgr;
  ASSERT_TRUE(mgr.process_order(make_order("o", 1,
    {make_node("n1", 0, true), make_node("n2", 2, true)},
    {make_edge("e12", 1, true, "n1", "n2")}
  )).accepted);

  mgr.edge_entered("e12", 1);
  ASSERT_EQ(mgr.active_edge_states().size(), 1u);

  ASSERT_TRUE(mgr.edge_completed("e12", 1));

  // After completion: active_edges should be empty
  EXPECT_EQ(mgr.active_edge_states().size(), 0u);
  EXPECT_EQ(mgr.edge_states().size(), 0u);
}
