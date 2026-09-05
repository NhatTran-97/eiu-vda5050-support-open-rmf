#include <gtest/gtest.h>

#include "tb3_vda5050_bridge/order_session.hpp"

namespace {

using tb3_vda5050_bridge::DispatchKind;
using tb3_vda5050_bridge::OrderSession;

vda5050_msgs::msg::Node make_node(const std::string& id,
                                  uint32_t sequence_id,
                                  bool released,
                                  bool with_position) {
  vda5050_msgs::msg::Node node;
  node.node_id = id;
  node.sequence_id = sequence_id;
  node.released = released;
  node.node_description = id;
  if (with_position) {
    node.node_position.x = static_cast<double>(sequence_id);
    node.node_position.y = 0.0;
    node.node_position.theta = 0.0;
    node.node_position.map_id = "map";
    node.node_position_set = true;
  }
  return node;
}

vda5050_msgs::msg::Edge make_edge(const std::string& id,
                                  uint32_t sequence_id,
                                  const std::string& start,
                                  const std::string& end) {
  vda5050_msgs::msg::Edge edge;
  edge.edge_id = id;
  edge.sequence_id = sequence_id;
  edge.edge_description = id;
  edge.start_node_id = start;
  edge.end_node_id = end;
  edge.released = true;
  return edge;
}

TEST(OrderSessionTest, ReleasedActionOnlyNodeIsConsumedBeforeNavigationTarget) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-1";
  order.nodes.push_back(make_node("n0", 0, true, false));
  order.nodes.push_back(make_node("n1", 2, true, true));
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));

  session.start(order);
  const auto plan = session.plan_next_work();

  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_EQ(plan.immediate_events.size(), 1u);
  ASSERT_TRUE(plan.immediate_events[0].node_reached.has_value());
  EXPECT_EQ(plan.immediate_events[0].node_reached->node_id, "n0");

  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n1");
  ASSERT_TRUE(plan.target->incoming_edge.has_value());
  EXPECT_EQ(plan.target->incoming_edge->edge_id, "e01");
}

TEST(OrderSessionTest, UnreleasedNodeStopsDispatchInsteadOfSkippingAhead) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-2";
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, false, true));
  order.nodes.push_back(make_node("n2", 4, true, true));
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  order.edges.push_back(make_edge("e12", 3, "n1", "n2"));

  session.start(order);

  auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n0");

  const auto completion = session.complete_navigation(plan.target->node_index);
  ASSERT_EQ(completion.size(), 1u);
  ASSERT_TRUE(completion[0].node_reached.has_value());
  EXPECT_EQ(completion[0].node_reached->node_id, "n0");

  plan = session.plan_next_work();
  EXPECT_EQ(plan.kind, DispatchKind::WAITING_FOR_RELEASE);
  EXPECT_FALSE(plan.target.has_value());
}

TEST(OrderSessionTest, CompletingNavigationPublishesEdgeCompletedAndNodeReached) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-3";
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, true, false));
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));

  session.start(order);

  auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());

  auto completion = session.complete_navigation(plan.target->node_index);
  ASSERT_EQ(completion.size(), 1u);
  ASSERT_TRUE(completion[0].node_reached.has_value());
  EXPECT_EQ(completion[0].node_reached->node_id, "n0");

  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::COMPLETED);
  ASSERT_EQ(plan.immediate_events.size(), 1u);
  ASSERT_TRUE(plan.immediate_events[0].edge_entered.has_value());
  ASSERT_TRUE(plan.immediate_events[0].edge_completed.has_value());
  ASSERT_TRUE(plan.immediate_events[0].node_reached.has_value());
  EXPECT_EQ(plan.immediate_events[0].edge_entered->edge_id, "e01");
  EXPECT_EQ(plan.immediate_events[0].node_reached->node_id, "n1");
}

TEST(OrderSessionTest, UpdateReleasingHorizonNodeResumesWithoutResettingProgress) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-u1";
  order.order_update_id = 0;
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, false, true));  // horizon (unreleased)
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  session.start(order);

  // Drive past n0; n1 is still horizon so the route blocks.
  auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  session.complete_navigation(plan.target->node_index);

  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::WAITING_FOR_RELEASE);

  const auto index_before = session.current_node_index();
  const auto generation_before = session.generation();

  // Order update (same orderId): the stitch node n0 is echoed back and n1 is now
  // released. Progress and generation must be preserved (in-flight nav untouched).
  vda5050_msgs::msg::Order update;
  update.order_id = "ord-u1";
  update.order_update_id = 1;
  update.nodes.push_back(make_node("n0", 0, true, true));
  update.nodes.push_back(make_node("n1", 2, true, true));  // released by the update
  update.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  session.update(update);

  EXPECT_EQ(session.current_node_index(), index_before);
  EXPECT_EQ(session.generation(), generation_before);
  EXPECT_EQ(session.order_id(), "ord-u1");

  // n1 is now navigable without having restarted from the beginning.
  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n1");
}

TEST(OrderSessionTest, UpdateAppendsNewNodesBeyondTheCurrentRoute) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-u2";
  order.order_update_id = 0;
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, true, true));
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  session.start(order);

  const auto generation_before = session.generation();

  // Reach n0.
  auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  session.complete_navigation(plan.target->node_index);

  // Update appends n2 past the last base node n1 (stitch node = n1).
  vda5050_msgs::msg::Order update;
  update.order_id = "ord-u2";
  update.order_update_id = 1;
  update.nodes.push_back(make_node("n1", 2, true, true));  // stitch (last base node)
  update.nodes.push_back(make_node("n2", 4, true, true));  // newly appended
  update.edges.push_back(make_edge("e12", 3, "n1", "n2"));
  session.update(update);

  EXPECT_EQ(session.generation(), generation_before);

  // The route now continues n1 -> n2 without re-traversing n0.
  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n1");
  session.complete_navigation(plan.target->node_index);

  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n2");
  ASSERT_TRUE(plan.target->incoming_edge.has_value());
  EXPECT_EQ(plan.target->incoming_edge->edge_id, "e12");
}

TEST(OrderSessionTest, UpdateWithNonNewerUpdateIdIsRejectedAndIgnored) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-stale";
  order.order_update_id = 5;
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, false, true));  // horizon
  session.start(order);

  // Drive past n0 so n1's release state is what's actually under test.
  auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  session.complete_navigation(plan.target->node_index);

  // A duplicate of the same updateId, and one that's actually older, must
  // both be rejected without touching state — n1 stays un-released.
  vda5050_msgs::msg::Order duplicate = order;
  duplicate.nodes[1].released = true;
  EXPECT_FALSE(session.update(duplicate));

  vda5050_msgs::msg::Order older = order;
  older.order_update_id = 3;
  older.nodes[1].released = true;
  EXPECT_FALSE(session.update(older));

  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::WAITING_FOR_RELEASE);

  // A genuinely newer updateId is accepted.
  vda5050_msgs::msg::Order newer = order;
  newer.order_update_id = 6;
  newer.nodes[1].released = true;
  EXPECT_TRUE(session.update(newer));

  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
}

TEST(OrderSessionTest, UpdateCannotRewriteContentOfAnAlreadyTraversedNode) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-base";
  order.order_update_id = 0;
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, true, true));
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  session.start(order);

  // Reach n0 — it (and edge e01 leading out of it) is now the traversed base.
  auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  ASSERT_EQ(plan.target->node.node_id, "n0");
  session.complete_navigation(plan.target->node_index);
  ASSERT_EQ(session.current_node_index(), 1u);

  // A buggy/stale update echoes n0 back with different content under the
  // same sequence_id — this must not silently rewrite already-driven history.
  vda5050_msgs::msg::Order update;
  update.order_id = "ord-base";
  update.order_update_id = 1;
  auto tampered_n0 = make_node("n0-tampered", 0, true, true);
  tampered_n0.node_position.x = 999.0;
  update.nodes.push_back(tampered_n0);
  update.nodes.push_back(make_node("n1", 2, true, true));
  update.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  ASSERT_TRUE(session.update(update));

  ASSERT_EQ(session.nodes().size(), 2u);
  EXPECT_EQ(session.nodes()[0].node_id, "n0");
  EXPECT_DOUBLE_EQ(session.nodes()[0].node_position.x, 0.0);

  plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n1");
}

TEST(OrderSessionTest, StartWithResumeCursorSkipsAlreadyTraversedNodes) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-resume";
  order.nodes.push_back(make_node("n0", 0, true, true));
  order.nodes.push_back(make_node("n1", 2, true, true));
  order.nodes.push_back(make_node("n2", 4, true, true));
  order.edges.push_back(make_edge("e01", 1, "n0", "n1"));
  order.edges.push_back(make_edge("e12", 3, "n1", "n2"));

  session.start(order, 1);
  EXPECT_EQ(session.current_node_index(), 1u);

  const auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  ASSERT_TRUE(plan.target.has_value());
  EXPECT_EQ(plan.target->node.node_id, "n1");
}

TEST(OrderSessionTest, NextNodeRequiresPoseToCompleteReflectsCurrentNode) {
  OrderSession session;

  // No order at all: nothing to require.
  EXPECT_FALSE(session.next_node_requires_pose_to_complete());

  vda5050_msgs::msg::Order order;
  order.order_id = "ord-pose";
  order.nodes.push_back(make_node("n0", 0, true, false));   // released, position-less
  order.nodes.push_back(make_node("n1", 2, true, true));    // released, has position
  session.start(order);

  EXPECT_TRUE(session.next_node_requires_pose_to_complete());

  const auto plan = session.plan_next_work();
  ASSERT_EQ(plan.kind, DispatchKind::NAVIGATE);
  EXPECT_EQ(plan.target->node.node_id, "n1");
  // Cursor advanced past the position-less node onto one that has a
  // position — nothing left that would be auto-completed on trust alone.
  EXPECT_FALSE(session.next_node_requires_pose_to_complete());
}

TEST(OrderSessionTest, NextNodeRequiresPoseToCompleteIsFalseForUnreleasedOrPositionedNode) {
  OrderSession session;

  vda5050_msgs::msg::Order order;
  order.order_id = "ord-pose-2";
  order.nodes.push_back(make_node("n0", 0, true, true));    // has position
  session.start(order);
  EXPECT_FALSE(session.next_node_requires_pose_to_complete());

  vda5050_msgs::msg::Order order2;
  order2.order_id = "ord-pose-3";
  order2.nodes.push_back(make_node("n0", 0, false, false)); // unreleased
  session.start(order2);
  EXPECT_FALSE(session.next_node_requires_pose_to_complete());
}

TEST(OrderSessionTest, StartClampsResumeCursorBeyondNodeCount) {
  OrderSession session;
  vda5050_msgs::msg::Order order;
  order.order_id = "ord-resume-2";
  order.nodes.push_back(make_node("n0", 0, true, true));

  session.start(order, 99);
  EXPECT_EQ(session.current_node_index(), 1u);

  const auto plan = session.plan_next_work();
  EXPECT_EQ(plan.kind, DispatchKind::COMPLETED);
}

}  // namespace
