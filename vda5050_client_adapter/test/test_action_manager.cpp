/**
 * @file test_action_manager.cpp
 * @brief Unit tests for vda5050_adapter::ActionManager.
 *
 * Coverage:
 *  - Order actions triggered by node/edge events
 *  - Instant actions dispatched immediately
 *  - NONE / SOFT / HARD blocking semantics
 *  - pause_all / resume_all (with exclude_id)
 *  - cancel_all (with exclude_id)
 *  - sync_order_actions (add new, remove stale, preserve active)
 *  - reset_for_new_order (keeps only running instant actions)
 *  - Edge actions cancelled when edge is left before completion
 *  - Feedback: set_action_running/finished/failed/paused
 *  - State queries: action_states, has_active_actions, is_hard_blocked, is_soft_blocked
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "vda5050_client_adapter/action_manager.hpp"

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

vda5050::Node make_node(const std::string& id, uint32_t seq,
                        std::vector<vda5050::Action> actions) {
  vda5050::Node n;
  n.node_id     = id;
  n.sequence_id = seq;
  n.released    = true;
  n.actions     = std::move(actions);
  return n;
}

vda5050::Edge make_edge(const std::string& id, uint32_t seq,
                        std::vector<vda5050::Action> actions) {
  vda5050::Edge e;
  e.edge_id       = id;
  e.sequence_id   = seq;
  e.released      = true;
  e.start_node_id = "n1";
  e.end_node_id   = "n2";
  e.actions       = std::move(actions);
  return e;
}

vda5050::InstantActions make_instant(std::vector<vda5050::Action> actions) {
  vda5050::InstantActions ia;
  ia.actions = std::move(actions);
  return ia;
}

vda5050::ActionStatus status_of(const vda5050_adapter::ActionManager& mgr,
                                const std::string& id) {
  for (const auto& s : mgr.action_states()) {
    if (s.action_id == id) return s.action_status;
  }
  return vda5050::ActionStatus::FAILED;
}

bool has_action(const vda5050_adapter::ActionManager& mgr, const std::string& id) {
  // NOTE: action_states() must be stored first — calling it twice yields two
  // separate temporaries whose iterators cannot be mixed safely.
  const auto states = mgr.action_states();
  return std::any_of(states.begin(), states.end(),
    [&](const auto& s){ return s.action_id == id; });
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Instant actions
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, InstantActionsDispatchedImmediately) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.process_instant_actions(make_instant({
    make_action("ia1"), make_action("ia2")
  }));

  ASSERT_EQ(executed.size(), 2u);
  EXPECT_EQ(executed[0], "ia1");
  EXPECT_EQ(executed[1], "ia2");
  EXPECT_EQ(status_of(mgr, "ia1"), vda5050::ActionStatus::INITIALIZING);
  EXPECT_EQ(status_of(mgr, "ia2"), vda5050::ActionStatus::INITIALIZING);
}

TEST(ActionManagerTest, DuplicateInstantActionIdIgnoredOnSecondCall) {
  vda5050_adapter::ActionManager mgr;
  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.process_instant_actions(make_instant({make_action("ia1")}));
  ASSERT_EQ(executed.size(), 1u);
  executed.clear();

  mgr.process_instant_actions(make_instant({make_action("ia1")}));
  EXPECT_TRUE(executed.empty());  // duplicate ignored
}

// ─────────────────────────────────────────────────────────────────────────────
// Order actions — trigger on node / edge
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, NodeActionsWaitUntilNodeReached) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.sync_order_actions(
    {make_node("n1", 0, {make_action("na1")}),
     make_node("n2", 2, {make_action("na2")})},
    {});

  EXPECT_TRUE(executed.empty());
  EXPECT_EQ(status_of(mgr, "na1"), vda5050::ActionStatus::WAITING);

  mgr.on_node_reached("n1", 0);
  ASSERT_EQ(executed, std::vector<std::string>{"na1"});
  EXPECT_EQ(status_of(mgr, "na1"), vda5050::ActionStatus::INITIALIZING);
  EXPECT_EQ(status_of(mgr, "na2"), vda5050::ActionStatus::WAITING);  // n2 not reached yet
}

TEST(ActionManagerTest, EdgeActionsWaitUntilEdgeEntered) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.sync_order_actions({}, {make_edge("e12", 1, {make_action("ea1")})});

  EXPECT_TRUE(executed.empty());

  mgr.on_edge_entered("e12", 1);
  ASSERT_EQ(executed, std::vector<std::string>{"ea1"});
}

TEST(ActionManagerTest, EdgeActionsFailedWhenEdgeLeftBeforeCompletion) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> cancelled;
  mgr.set_execute_callback([](const vda5050::Action&){});
  mgr.set_cancel_callback([&](const std::string& id){ cancelled.push_back(id); });

  mgr.sync_order_actions({}, {make_edge("e12", 1, {make_action("ea1")})});
  mgr.on_edge_entered("e12", 1);
  mgr.set_action_running("ea1");

  // Edge left before action finishes
  mgr.on_edge_left("e12", 1);

  EXPECT_EQ(status_of(mgr, "ea1"), vda5050::ActionStatus::FAILED);
  ASSERT_EQ(cancelled, std::vector<std::string>{"ea1"});
}

TEST(ActionManagerTest, EdgeActionsNotFailedIfAlreadyFinished) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> cancelled;
  mgr.set_execute_callback([](const vda5050::Action&){});
  mgr.set_cancel_callback([&](const std::string& id){ cancelled.push_back(id); });

  mgr.sync_order_actions({}, {make_edge("e12", 1, {make_action("ea1")})});
  mgr.on_edge_entered("e12", 1);
  mgr.set_action_running("ea1");
  mgr.set_action_finished("ea1", "done");

  mgr.on_edge_left("e12", 1);

  EXPECT_TRUE(cancelled.empty());  // already done, not cancelled
  EXPECT_EQ(status_of(mgr, "ea1"), vda5050::ActionStatus::FINISHED);
}

// ─────────────────────────────────────────────────────────────────────────────
// NONE blocking
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, NoneBlockingActionsRunConcurrently) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.process_instant_actions(make_instant({
    make_action("a1", "t", vda5050::BlockingType::NONE),
    make_action("a2", "t", vda5050::BlockingType::NONE),
  }));

  ASSERT_EQ(executed.size(), 2u);
  EXPECT_FALSE(mgr.is_hard_blocked());
  EXPECT_FALSE(mgr.is_soft_blocked());
}

// ─────────────────────────────────────────────────────────────────────────────
// SOFT blocking
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, SoftBlockingActionSetsSoftBlockedFlag) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  mgr.process_instant_actions(make_instant({
    make_action("soft1", "t", vda5050::BlockingType::SOFT)
  }));
  mgr.set_action_running("soft1");

  EXPECT_TRUE(mgr.is_soft_blocked());
  EXPECT_FALSE(mgr.is_hard_blocked());

  mgr.set_action_finished("soft1", "ok");
  EXPECT_FALSE(mgr.is_soft_blocked());
}

TEST(ActionManagerTest, SoftBlockingActionDoesNotBlockParallelNoneActions) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.process_instant_actions(make_instant({
    make_action("soft1", "t", vda5050::BlockingType::SOFT),
    make_action("none1", "t", vda5050::BlockingType::NONE),
  }));

  ASSERT_EQ(executed.size(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// HARD blocking
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, HardActionPausesExistingActionsAndRunsAlone) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed, paused, resumed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });
  mgr.set_pause_callback  ([&](const std::string& id)  { paused.push_back(id); });
  mgr.set_resume_callback ([&](const std::string& id)  { resumed.push_back(id); });

  // Start a NONE action first
  mgr.process_instant_actions(make_instant({make_action("none1")}));
  mgr.set_action_running("none1");
  executed.clear();

  // Now enqueue a HARD action
  mgr.process_instant_actions(make_instant({
    make_action("hard1", "t", vda5050::BlockingType::HARD)
  }));

  // hard1 must NOT execute yet; none1 must be paused
  EXPECT_TRUE(executed.empty());
  ASSERT_EQ(paused, std::vector<std::string>{"none1"});
  EXPECT_EQ(status_of(mgr, "hard1"), vda5050::ActionStatus::WAITING);

  // Robot confirms none1 is paused → hard1 should dispatch
  mgr.set_action_paused("none1");
  ASSERT_EQ(executed, std::vector<std::string>{"hard1"});
  EXPECT_TRUE(mgr.is_hard_blocked());

  // hard1 finishes → none1 should be resumed
  mgr.set_action_running("hard1");
  mgr.set_action_finished("hard1", "ok");

  ASSERT_EQ(resumed, std::vector<std::string>{"none1"});
  EXPECT_FALSE(mgr.is_hard_blocked());
}

TEST(ActionManagerTest, HardActionSetsHardBlockedFlagWhileRunning) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  mgr.process_instant_actions(make_instant({
    make_action("h1", "t", vda5050::BlockingType::HARD)
  }));
  mgr.set_action_running("h1");

  EXPECT_TRUE(mgr.is_hard_blocked());
  EXPECT_FALSE(mgr.is_soft_blocked());

  mgr.set_action_failed("h1", "err");
  EXPECT_FALSE(mgr.is_hard_blocked());
}

// ─────────────────────────────────────────────────────────────────────────────
// pause_all / resume_all
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, PauseAllSendsRequestToRunningActionsExceptExcluded) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> paused;
  mgr.set_execute_callback([](const vda5050::Action&){});
  mgr.set_pause_callback([&](const std::string& id){ paused.push_back(id); });

  mgr.process_instant_actions(make_instant({
    make_action("ctrl"), make_action("work1"), make_action("work2")
  }));
  mgr.set_action_running("ctrl");
  mgr.set_action_running("work1");
  mgr.set_action_running("work2");
  paused.clear();

  mgr.pause_all("ctrl");  // exclude ctrl

  ASSERT_EQ(paused.size(), 2u);
  EXPECT_TRUE(std::find(paused.begin(), paused.end(), "work1") != paused.end());
  EXPECT_TRUE(std::find(paused.begin(), paused.end(), "work2") != paused.end());
  EXPECT_TRUE(std::find(paused.begin(), paused.end(), "ctrl") == paused.end());
}

TEST(ActionManagerTest, ResumeAllSendsRequestToPausedActionsExceptExcluded) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> resumed;
  mgr.set_execute_callback([](const vda5050::Action&){});
  mgr.set_pause_callback([](const std::string&){});
  mgr.set_resume_callback([&](const std::string& id){ resumed.push_back(id); });

  mgr.process_instant_actions(make_instant({
    make_action("ctrl"), make_action("work")
  }));
  mgr.set_action_running("ctrl");
  mgr.set_action_running("work");
  mgr.pause_all("ctrl");
  mgr.set_action_paused("work");
  resumed.clear();

  mgr.resume_all("ctrl");
  ASSERT_EQ(resumed, std::vector<std::string>{"work"});
}

TEST(ActionManagerTest, PauseAllSuppressesNewOrderActionDispatches) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.process_instant_actions(make_instant({make_action("ctrl")}));
  mgr.set_action_running("ctrl");
  mgr.pause_all("ctrl");
  executed.clear();

  // Order actions enqueued while paused must NOT dispatch
  mgr.sync_order_actions({make_node("n1", 0, {make_action("na1")})}, {});
  mgr.on_node_reached("n1", 0);

  EXPECT_TRUE(executed.empty());
  EXPECT_EQ(status_of(mgr, "na1"), vda5050::ActionStatus::WAITING);
}

// ─────────────────────────────────────────────────────────────────────────────
// cancel_all
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, CancelAllFailsAllNonTerminalActionsExceptExcluded) {
  vda5050_adapter::ActionManager mgr;

  std::vector<std::string> cancelled;
  mgr.set_execute_callback([](const vda5050::Action&){});
  mgr.set_cancel_callback([&](const std::string& id){ cancelled.push_back(id); });

  mgr.process_instant_actions(make_instant({
    make_action("ctrl"), make_action("running")
  }));
  mgr.set_action_running("ctrl");
  mgr.set_action_running("running");

  mgr.sync_order_actions({make_node("n1", 0, {make_action("waiting")})}, {});
  cancelled.clear();

  mgr.cancel_all("ctrl");

  // "running" cancelled, "waiting" failed, "ctrl" untouched
  EXPECT_EQ(status_of(mgr, "ctrl"),    vda5050::ActionStatus::RUNNING);
  EXPECT_EQ(status_of(mgr, "running"), vda5050::ActionStatus::FAILED);
  EXPECT_EQ(status_of(mgr, "waiting"), vda5050::ActionStatus::FAILED);
  ASSERT_EQ(cancelled, std::vector<std::string>{"running"});
}

TEST(ActionManagerTest, CancelAllDoesNotTouchAlreadyFinishedActions) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  mgr.process_instant_actions(make_instant({make_action("done")}));
  mgr.set_action_running("done");
  mgr.set_action_finished("done", "ok");

  mgr.cancel_all();
  EXPECT_EQ(status_of(mgr, "done"), vda5050::ActionStatus::FINISHED);
}

// ─────────────────────────────────────────────────────────────────────────────
// sync_order_actions
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, SyncOrderActionsAddsNewActionsAndPreservesActive) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  mgr.sync_order_actions({make_node("n1", 0, {make_action("act1")})}, {});
  mgr.on_node_reached("n1", 0);
  mgr.set_action_running("act1");

  // Stitch: add n2 actions, keep act1 running
  mgr.sync_order_actions(
    {make_node("n1", 0, {make_action("act1")}),
     make_node("n2", 2, {make_action("act2")})},
    {});

  EXPECT_EQ(status_of(mgr, "act1"), vda5050::ActionStatus::RUNNING);  // preserved
  EXPECT_EQ(status_of(mgr, "act2"), vda5050::ActionStatus::WAITING);  // new
}

TEST(ActionManagerTest, SyncOrderActionsRemovesStaleWaitingActions) {
  vda5050_adapter::ActionManager mgr;

  mgr.sync_order_actions({make_node("n1", 0, {make_action("stale")})}, {});
  EXPECT_TRUE(has_action(mgr, "stale"));

  // New sync without n1 → "stale" was WAITING and not triggered → removed
  mgr.sync_order_actions({make_node("n2", 2, {make_action("fresh")})}, {});
  EXPECT_FALSE(has_action(mgr, "stale"));
  EXPECT_TRUE(has_action(mgr, "fresh"));
}

TEST(ActionManagerTest, SyncOrderActionsPreservesTriggeredWaitingActions) {
  vda5050_adapter::ActionManager mgr;
  std::vector<std::string> executed;
  mgr.set_execute_callback([&](const vda5050::Action& a){ executed.push_back(a.action_id); });

  mgr.sync_order_actions({make_node("n1", 0, {make_action("triggered")})}, {});
  mgr.on_node_reached("n1", 0);
  // triggered is now INITIALIZING (trigger_ready=true)

  mgr.sync_order_actions({make_node("n2", 2, {make_action("fresh")})}, {});

  // "triggered" must survive even though n1 is no longer in the sync set
  EXPECT_TRUE(has_action(mgr, "triggered"));
}

// ─────────────────────────────────────────────────────────────────────────────
// reset_for_new_order
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, ResetForNewOrderKeepsRunningInstantActionsOnly) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  // Running instant
  mgr.process_instant_actions(make_instant({make_action("running_instant")}));
  mgr.set_action_running("running_instant");

  // Waiting order action
  mgr.sync_order_actions({make_node("n1", 0, {make_action("order_waiting")})}, {});

  // Finished instant
  mgr.process_instant_actions(make_instant({make_action("done_instant")}));
  mgr.set_action_running("done_instant");
  mgr.set_action_finished("done_instant", "ok");

  mgr.reset_for_new_order();

  EXPECT_TRUE(has_action(mgr, "running_instant"));   // kept
  EXPECT_FALSE(has_action(mgr, "order_waiting"));    // removed
  EXPECT_FALSE(has_action(mgr, "done_instant"));     // removed (finished)
}

// ─────────────────────────────────────────────────────────────────────────────
// has_active_actions / state queries
// ─────────────────────────────────────────────────────────────────────────────

TEST(ActionManagerTest, HasActiveActionsReturnsTrueForInitializingRunningPaused) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  EXPECT_FALSE(mgr.has_active_actions());

  mgr.process_instant_actions(make_instant({make_action("a1")}));
  EXPECT_TRUE(mgr.has_active_actions());  // INITIALIZING

  mgr.set_action_running("a1");
  EXPECT_TRUE(mgr.has_active_actions());  // RUNNING

  mgr.pause_all();
  mgr.set_action_paused("a1");
  EXPECT_TRUE(mgr.has_active_actions());  // PAUSED

  mgr.set_action_finished("a1", "ok");
  EXPECT_FALSE(mgr.has_active_actions());  // FINISHED → inactive
}

TEST(ActionManagerTest, ActionStatesPreservesInsertionOrder) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  mgr.process_instant_actions(make_instant({
    make_action("first"), make_action("second"), make_action("third")
  }));

  const auto states = mgr.action_states();
  ASSERT_EQ(states.size(), 3u);
  EXPECT_EQ(states[0].action_id, "first");
  EXPECT_EQ(states[1].action_id, "second");
  EXPECT_EQ(states[2].action_id, "third");
}

TEST(ActionManagerTest, SetActionFinishedStoresResultDescription) {
  vda5050_adapter::ActionManager mgr;
  mgr.set_execute_callback([](const vda5050::Action&){});

  mgr.process_instant_actions(make_instant({make_action("a1")}));
  mgr.set_action_running("a1");
  mgr.set_action_finished("a1", "load placed successfully");

  for (const auto& s : mgr.action_states()) {
    if (s.action_id == "a1") {
      EXPECT_EQ(s.result_description, "load placed successfully");
    }
  }
}

TEST(ActionManagerTest, UnknownActionIdInFeedbackIsHandledGracefully) {
  vda5050_adapter::ActionManager mgr;
  // Should not throw/crash
  EXPECT_NO_THROW(mgr.set_action_running("does-not-exist"));
  EXPECT_NO_THROW(mgr.set_action_finished("does-not-exist", ""));
  EXPECT_NO_THROW(mgr.set_action_failed("does-not-exist",  ""));
  EXPECT_NO_THROW(mgr.set_action_paused("does-not-exist"));
}

TEST(ActionManagerTest, IdleManagerHasNoActiveActions) {
  vda5050_adapter::ActionManager mgr;
  EXPECT_FALSE(mgr.has_active_actions());
  EXPECT_FALSE(mgr.is_hard_blocked());
  EXPECT_FALSE(mgr.is_soft_blocked());
  EXPECT_TRUE(mgr.action_states().empty());
}
