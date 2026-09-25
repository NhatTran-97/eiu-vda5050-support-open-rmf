/**
 * @file test_full_control_compat.cpp
 * @brief Wire compatibility with vda5050_fleet_adapter_full_control.
 *
 * The fleet adapter's own message builders and parsers are compiled in from its source tree.
 *
 * Offline:
 *  - its orders, order updates and instant actions parse and are accepted by the client
 *  - the client's state and factsheet parse in its ParsedState / ParsedFactsheet / CancelTracker
 *  - node shutdown while the broker is unreachable
 *
 * Live (VDA5050_TEST_BROKER=tcp://host:port), default and strict mode:
 *  a VDA5050Node, a fake bridge (NavigateToNode server) and the fleet adapter side on MQTT —
 *  route with horizon release, cancel + new order, pause/resume, edge and blocking actions,
 *  malformed/resent orders, driver restart, failed/dropped/abandoned steps, latched operating mode,
 *  factsheetRequest, state coalescing, concurrent traffic.
 */

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <rmw/rmw.h>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <vda5050_msgs/action/navigate_to_node.hpp>
#include <vda5050_msgs/msg/action.hpp>
#include <vda5050_msgs/msg/action_command.hpp>
#include <vda5050_msgs/msg/action_state.hpp>
#include <vda5050_msgs/msg/agv_position.hpp>
#include <vda5050_msgs/msg/driver_status.hpp>
#include <vda5050_msgs/msg/error.hpp>
#include <vda5050_msgs/msg/node_state.hpp>

#include "vda5050_client_adapter/action_manager.hpp"
#include "vda5050_client_adapter/json_converter.hpp"
#include "vda5050_client_adapter/mqtt_client.hpp"
#include "vda5050_client_adapter/order_manager.hpp"
#include "vda5050_client_adapter/vda5050_node.hpp"

#include "vda5050_fleet_adapter_full_control/vda5050/cancel_tracker.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

namespace fc = vda5050_fleet_adapter_full_control::vda5050;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// ─── Shared helpers ──────────────────────────────────────────────────────────

class RosEnvironment : public ::testing::Environment {
public:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};
const auto* const kRosEnvironment = ::testing::AddGlobalTestEnvironment(new RosEnvironment);

// Same checks as has_required_state_fields() in full_control's connector.cpp.
bool has_required_state_fields(const json& raw)
{
  return raw.is_object() &&
         raw.contains("orderId") && raw["orderId"].is_string() &&
         raw.contains("lastNodeId") && raw["lastNodeId"].is_string() &&
         raw.contains("driving") && raw["driving"].is_boolean() &&
         raw.contains("nodeStates") && raw["nodeStates"].is_array() &&
         raw.contains("edgeStates") && raw["edgeStates"].is_array() &&
         raw.contains("actionStates") && raw["actionStates"].is_array() &&
         raw.contains("errors") && raw["errors"].is_array() &&
         raw.contains("operatingMode") && raw["operatingMode"].is_string() &&
         raw.contains("safetyState") && raw["safetyState"].is_object() &&
         raw["safetyState"].contains("eStop") && raw["safetyState"]["eStop"].is_string() &&
         raw["safetyState"].contains("fieldViolation") && raw["safetyState"]["fieldViolation"].is_boolean() &&
         raw.contains("batteryState") && raw["batteryState"].is_object() &&
         raw["batteryState"].contains("batteryCharge") && raw["batteryState"]["batteryCharge"].is_number();
}

// Route wp0 -> wp1 -> wp2 -> wp3 along x.
std::vector<fc::RouteWaypoint> route()
{
  std::vector<fc::RouteWaypoint> points;
  for (int i = 1; i <= 3; ++i)
  {
    points.push_back({"wp" + std::to_string(i), {1.0 * i, 0.0, 0.0}, std::nullopt});
  }
  return points;
}

// full_control's first order of a path: base node wp0 plus the route, released_count points released.
json first_order(const std::string& order_id, const std::string& manufacturer, const std::string& serial,
                 std::size_t released_count)
{
  return fc::build_route_order(1, order_id, manufacturer, serial, "wp0", {0.0, 0.0, 0.0}, route(), "map",
                               0, released_count, 0);
}

// full_control's release_more(): update stitched at the last released node.
json release_update(const std::string& order_id, const std::string& manufacturer, const std::string& serial,
                    int update_id, std::size_t sent_released, std::size_t released_count)
{
  return fc::build_route_order(2, order_id, manufacturer, serial, "wp0", {0.0, 0.0, 0.0}, route(), "map",
                               update_id, released_count, sent_released);
}

bool has_error(const json& state, const std::string& type)
{
  return std::any_of(state["errors"].begin(), state["errors"].end(),
                     [&](const json& e) { return e.value("errorType", "") == type; });
}

std::optional<std::string> action_status(const json& state, const std::string& action_id)
{
  for (const auto& a : state["actionStates"])
  {
    if (a.value("actionId", "") == action_id) return a.value("actionStatus", "");
  }
  return std::nullopt;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 5s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Offline: full_control messages into the client managers
// ─────────────────────────────────────────────────────────────────────────────

class OfflineCompat : public ::testing::TestWithParam<bool> {};

TEST_P(OfflineCompat, RouteOrderAndReleaseUpdatesAreAcceptedAndTraversed)
{
  vda5050_adapter::OrderManager mgr;
  mgr.set_strict_mode(GetParam());

  const auto first = first_order("o1", "M", "S", 2).get<vda5050::Order>();
  EXPECT_EQ(vda5050_adapter::OrderManager::validate_structure(first), "");
  const auto r0 = mgr.process_order(first);
  ASSERT_TRUE(r0.accepted) << r0.rejection_reason;

  ASSERT_TRUE(mgr.node_reached({"wp0", 0, 0.0}));
  ASSERT_TRUE(mgr.edge_entered("e_wp0_wp1", 1));
  ASSERT_TRUE(mgr.edge_completed("e_wp0_wp1", 1));
  ASSERT_TRUE(mgr.node_reached({"wp1", 2, 1.0}));
  EXPECT_TRUE(mgr.new_base_request());

  const auto update = release_update("o1", "M", "S", 1, 2, 3).get<vda5050::Order>();
  EXPECT_EQ(vda5050_adapter::OrderManager::validate_structure(update), "");
  const auto r1 = mgr.process_order(update);
  ASSERT_TRUE(r1.accepted) << r1.rejection_reason;
  EXPECT_FALSE(mgr.new_base_request());

  ASSERT_TRUE(mgr.node_reached({"wp2", 4, 1.0}));
  ASSERT_TRUE(mgr.node_reached({"wp3", 6, 1.0}));
  EXPECT_FALSE(mgr.has_active_order());
  EXPECT_EQ(mgr.last_node_sequence_id(), 6u);
}

TEST_P(OfflineCompat, UpdateAfterTheBaseIsUsedUpStitchesAtTheLastTraversedNode)
{
  vda5050_adapter::OrderManager mgr;
  mgr.set_strict_mode(GetParam());
  ASSERT_TRUE(mgr.process_order(first_order("o1", "M", "S", 1).get<vda5050::Order>()).accepted);
  ASSERT_TRUE(mgr.node_reached({"wp0", 0, 0.0}));
  ASSERT_TRUE(mgr.node_reached({"wp1", 2, 1.0}));
  ASSERT_TRUE(mgr.has_active_order());

  const auto r = mgr.process_order(release_update("o1", "M", "S", 1, 1, 3).get<vda5050::Order>());
  ASSERT_TRUE(r.accepted) << r.rejection_reason;
  EXPECT_EQ(mgr.node_states().size(), 2u);
}

TEST_P(OfflineCompat, SkippedUpdateIdsAreAccepted)
{
  vda5050_adapter::OrderManager mgr;
  mgr.set_strict_mode(GetParam());
  ASSERT_TRUE(mgr.process_order(first_order("o1", "M", "S", 1).get<vda5050::Order>()).accepted);
  const auto r = mgr.process_order(release_update("o1", "M", "S", 3, 1, 2).get<vda5050::Order>());
  EXPECT_TRUE(r.accepted) << r.rejection_reason;
}

INSTANTIATE_TEST_SUITE_P(Modes, OfflineCompat, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Strict" : "Default"; });

TEST(OfflineCompatTest, FullControlInstantActionsParse)
{
  const std::vector<json> messages = {
    fc::build_cancel_order(1, "M", "S", "HARD"),
    fc::build_start_pause(2, "M", "S"),
    fc::build_stop_pause(3, "M", "S"),
    fc::build_state_request(4, "M", "S"),
    fc::build_instant_action(5, "M", "S", "factsheetRequest", json::object(), "NONE").message,
  };
  for (const auto& message : messages)
  {
    const auto ia = message.get<vda5050::InstantActions>();
    ASSERT_EQ(ia.actions.size(), 1u) << message.dump();
    EXPECT_FALSE(ia.actions[0].action_id.empty());
  }

  const auto init = fc::build_instant_action(6, "M", "S", "initPosition",
    {{"x", 1.5}, {"y", -2.25}, {"theta", 0.3}, {"mapId", "map"}}, "NONE");
  const auto action = init.message.get<vda5050::InstantActions>().actions.at(0);
  EXPECT_EQ(action.action_id, init.action_id);
  std::map<std::string, std::string> params;
  for (const auto& p : action.action_parameters) params[p.key] = p.value;
  EXPECT_DOUBLE_EQ(std::stod(params.at("x")), 1.5);
  EXPECT_DOUBLE_EQ(std::stod(params.at("y")), -2.25);
  EXPECT_DOUBLE_EQ(std::stod(params.at("theta")), 0.3);
  EXPECT_EQ(params.at("mapId"), "map");
}

TEST(OfflineCompatTest, ClientStateParsesInFullControl)
{
  vda5050::State s;
  s.header.header_id = 7;
  s.header.timestamp = "2026-09-24T10:00:00.123Z";
  s.order_id = "o1";
  s.order_update_id = 2;
  s.last_node_id = "wp1";
  s.last_node_sequence_id = 2;
  s.driving = true;
  s.new_base_request = true;
  s.node_states.push_back({});
  s.node_states.back().node_id = "wp2";
  s.node_states.back().sequence_id = 4;
  s.battery_state.battery_charge = 80.0;
  vda5050::AgvPosition pos;
  pos.position_initialized = true;
  pos.x = 1.0;
  pos.y = 2.0;
  pos.theta = 0.5;
  pos.map_id = "map";
  s.agv_position = pos;

  const json raw = s;
  ASSERT_TRUE(has_required_state_fields(raw)) << raw.dump();
  const fc::ParsedState parsed(raw);
  EXPECT_EQ(parsed.header_id, 7u);
  ASSERT_TRUE(parsed.timestamp_ms.has_value());
  EXPECT_EQ(parsed.order_id, "o1");
  EXPECT_EQ(parsed.order_update_id, 2u);
  EXPECT_EQ(parsed.last_node_sequence_id, 2u);
  EXPECT_TRUE(parsed.has_position());
  EXPECT_NEAR(*parsed.battery_soc, 0.8, 1e-9);
  EXPECT_TRUE(parsed.new_base_request);
  EXPECT_TRUE(parsed.operable());
  EXPECT_FALSE(parsed.order_finished("o1", "wp2"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Node lifecycle without a broker
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeLifecycleTest, ShutdownWhileTheBrokerIsUnreachableIsClean)
{
  for (int round = 0; round < 3; ++round)
  {
    rclcpp::NodeOptions options;
    options.use_global_arguments(false);
    options.arguments({"--ros-args", "-r", "__ns:=/lifecycle_" + std::to_string(::getpid())});
    options.parameter_overrides({{"mqtt.broker_url", "tcp://127.0.0.1:1"},
                                 {"vda5050.serial_number", "lifecycle"}});
    auto node = std::make_shared<vda5050_adapter::VDA5050Node>(options);
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until) executor.spin_some(50ms);
    executor.remove_node(node);
    node.reset();
  }
  SUCCEED();
}

TEST(NodeLifecycleTest, InvalidParameterIsRejectedAtStartup)
{
  rclcpp::NodeOptions options;
  options.use_global_arguments(false);
  options.parameter_overrides({{"vda5050.event_loop_period", 0.0}});
  EXPECT_THROW(vda5050_adapter::VDA5050Node node(options), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Live: VDA5050Node + fake bridge (ROS) + fleet adapter side (MQTT)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Fleet adapter side: records state/connection/factsheet and publishes order/instantActions.
class MasterSide {
public:
  MasterSide(const std::string& broker, const std::string& prefix)
  : prefix_(prefix)
  {
    vda5050_adapter::MqttConfig cfg;
    cfg.broker_url = broker;
    cfg.client_id = "compat_master_" + std::to_string(::getpid()) + "_" + std::to_string(++instances_);
    cfg.clean_session = true;
    client_ = std::make_unique<vda5050_adapter::MqttClient>(cfg);
    client_->subscribe(prefix_ + "state", 0, [this](const auto& m) { record(states_, m.payload); });
    client_->subscribe(prefix_ + "connection", 1, [this](const auto& m) { record(connections_, m.payload); });
    client_->subscribe(prefix_ + "factsheet", 1, [this](const auto& m) { record(factsheets_, m.payload); });
    client_->connect();
  }

  // Stop MQTT callbacks before the recorded messages are destroyed.
  ~MasterSide() { client_.reset(); }

  bool connected() const { return client_->is_connected(); }
  void send_order(const json& order) { client_->publish(prefix_ + "order", order.dump(), 0, false); }
  void send_raw_order(const std::string& payload) { client_->publish(prefix_ + "order", payload, 0, false); }
  void send_instant(const json& ia) { client_->publish(prefix_ + "instantActions", ia.dump(), 0, false); }

  std::vector<json> states() const { std::lock_guard<std::mutex> l(mutex_); return states_; }
  std::vector<json> connections() const { std::lock_guard<std::mutex> l(mutex_); return connections_; }
  std::vector<json> factsheets() const { std::lock_guard<std::mutex> l(mutex_); return factsheets_; }
  std::size_t state_count() const { std::lock_guard<std::mutex> l(mutex_); return states_.size(); }

  std::optional<json> latest_state() const
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (states_.empty()) return std::nullopt;
    return std::optional<json>(std::in_place, states_.back());
  }

  // Wait for a state (received after the call) that satisfies predicate.
  template <typename Predicate>
  std::optional<json> wait_state(Predicate predicate, std::chrono::milliseconds timeout = 5s)
  {
    std::optional<json> match;
    wait_until([&] {
      const auto s = latest_state();
      if (s && predicate(*s)) { match = s; return true; }
      return false;
    }, timeout);
    return match;
  }

private:
  void record(std::vector<json>& into, const std::string& payload)
  {
    auto j = json::parse(payload, nullptr, false);
    std::lock_guard<std::mutex> l(mutex_);
    into.push_back(std::move(j));
  }

  static inline std::atomic<int> instances_{0};
  std::string prefix_;
  std::unique_ptr<vda5050_adapter::MqttClient> client_;
  mutable std::mutex mutex_;
  std::vector<json> states_;
  std::vector<json> connections_;
  std::vector<json> factsheets_;
};

// Fake tb3_vda5050_bridge: NavigateToNode server whose steps the test finishes, plus the driver topics.
class FakeBridge {
public:
  using NavigateToNode = vda5050_msgs::action::NavigateToNode;
  using StepHandle = rclcpp_action::ServerGoalHandle<NavigateToNode>;

  struct Goal {
    std::string order_id;
    uint32_t    order_update_id{0};
    std::string node_id;
    uint32_t    sequence_id{0};
    std::string edge_id;  // empty without an incoming edge
  };

  explicit FakeBridge(const std::string& ns)
  {
    rclcpp::NodeOptions options;
    options.use_global_arguments(false);
    options.arguments({"--ros-args", "-r", "__ns:=" + ns});
    node_ = std::make_shared<rclcpp::Node>("fake_bridge", options);
    const std::string base = ns + "/vda5050_client_adapter/";

    server_ = rclcpp_action::create_server<NavigateToNode>(
      node_, base + "navigate_to_node",
      [](const rclcpp_action::GoalUUID&, std::shared_ptr<const NavigateToNode::Goal>) {
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
      },
      [](std::shared_ptr<StepHandle>) { return rclcpp_action::CancelResponse::ACCEPT; },
      [this](std::shared_ptr<StepHandle> handle) { on_accepted(handle); });
    cancel_timer_ = node_->create_wall_timer(5ms, [this] { finish_cancels(); });

    command_sub_ = node_->create_subscription<vda5050_msgs::msg::ActionCommand>(
      base + "action_command", rclcpp::QoS(10),
      [this](vda5050_msgs::msg::ActionCommand::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); commands_.push_back(*m); });
    execute_sub_ = node_->create_subscription<vda5050_msgs::msg::Action>(
      base + "action_execute", rclcpp::QoS(10),
      [this](vda5050_msgs::msg::Action::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); executed_.push_back(*m); });

    // Local status as an on-robot tool (robot_local_ui) reads it.
    local_subs_.push_back(node_->create_subscription<std_msgs::msg::Bool>(
      base + "driving", rclcpp::QoS(1).transient_local(),
      [this](std_msgs::msg::Bool::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); local_driving_ = m->data; }));
    local_subs_.push_back(node_->create_subscription<std_msgs::msg::Bool>(
      base + "paused", rclcpp::QoS(1).transient_local(),
      [this](std_msgs::msg::Bool::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); local_paused_ = m->data; }));
    local_subs_.push_back(node_->create_subscription<vda5050_msgs::msg::NodeState>(
      base + "node_reached", rclcpp::QoS(10),
      [this](vda5050_msgs::msg::NodeState::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); local_nodes_.push_back(*m); }));
    local_subs_.push_back(node_->create_subscription<vda5050_msgs::msg::Error>(
      base + "error", rclcpp::QoS(10),
      [this](vda5050_msgs::msg::Error::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); local_errors_.push_back(*m); }));

    status_pub_ = node_->create_publisher<vda5050_msgs::msg::DriverStatus>(
      base + "driver_status",
      rclcpp::QoS(1).transient_local()
        .liveliness(rclcpp::LivelinessPolicy::ManualByTopic)
        .liveliness_lease_duration(rclcpp::Duration(kStatusLease)));
    status_timer_ = node_->create_wall_timer(kStatusLease / 3, [this] {
      std::lock_guard<std::mutex> l(mutex_);
      if (!hung_) publish_status();
    });
    mode_pub_ = node_->create_publisher<std_msgs::msg::String>(base + "operating_mode", rclcpp::QoS(1).transient_local());
    feedback_pub_ = node_->create_publisher<vda5050_msgs::msg::ActionState>(base + "action_state_feedback", rclcpp::QoS(10));
    position_pub_ = node_->create_publisher<vda5050_msgs::msg::AgvPosition>(base + "agv_position", rclcpp::QoS(10));
    publish_status();
  }

  rclcpp::Node::SharedPtr node() const { return node_; }

  // True once the client's subscriptions and publishers for the volatile topics are matched.
  bool linked() const
  {
    return feedback_pub_->get_subscription_count() > 0 && position_pub_->get_subscription_count() > 0 &&
           command_sub_->get_publisher_count() > 0 && execute_sub_->get_publisher_count() > 0;
  }

  void driving(bool on)
  {
    std::lock_guard<std::mutex> l(mutex_);
    driving_ = on;
    publish_status();
  }
  void operating_mode(const std::string& mode) { std_msgs::msg::String m; m.data = mode; mode_pub_->publish(m); }
  void feedback(const std::string& id, const std::string& status)
  {
    vda5050_msgs::msg::ActionState m; m.action_id = id; m.action_status = status; feedback_pub_->publish(m);
  }
  void position(double x)
  {
    vda5050_msgs::msg::AgvPosition m; m.x = x; m.map_id = "map"; m.position_initialized = true; position_pub_->publish(m);
  }

  // Report the incoming edge of the active step as entered.
  void enter()
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!active_ || !active_->is_active()) return;
    auto feedback = std::make_shared<NavigateToNode::Feedback>();
    feedback->edge_entered = true;
    active_->publish_feedback(feedback);
  }
  bool reach(double distance = 1.0) { return finish(NavigateToNode::Result::REACHED, distance, "node reached"); }
  bool fail(const std::string& reason) { return finish(NavigateToNode::Result::FAILED, 0.0, reason); }
  bool drop(const std::string& reason) { return finish(NavigateToNode::Result::DROPPED, 0.0, reason); }

  // Keep cancel requests pending until release_cancels().
  void hold_cancels(bool hold) { std::lock_guard<std::mutex> l(mutex_); hold_cancels_ = hold; }
  bool cancel_pending() const { std::lock_guard<std::mutex> l(mutex_); return active_ && active_->is_canceling(); }
  std::size_t cancelled() const { std::lock_guard<std::mutex> l(mutex_); return cancelled_; }
  std::size_t preempted() const { std::lock_guard<std::mutex> l(mutex_); return preempted_; }

  // Stop (true) or resume (false) the driver_status heartbeat, as a stalled driver process.
  void hang(bool hung) { std::lock_guard<std::mutex> l(mutex_); hung_ = hung; }

  // Destroy the active step's handle unfinished: the server cancels it with a default result.
  void abandon()
  {
    std::lock_guard<std::mutex> l(mutex_);
    active_.reset();
  }

  // New session id; the active step is lost without a result, as in a process restart.
  void restart()
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (active_) lost_.push_back(std::move(active_));
    driving_ = false;
    ++session_;
    publish_status();
  }

  bool local_driving() const { std::lock_guard<std::mutex> l(mutex_); return local_driving_; }
  bool local_paused() const { std::lock_guard<std::mutex> l(mutex_); return local_paused_; }
  std::vector<vda5050_msgs::msg::NodeState> local_nodes() const { std::lock_guard<std::mutex> l(mutex_); return local_nodes_; }
  std::vector<vda5050_msgs::msg::Error> local_errors() const { std::lock_guard<std::mutex> l(mutex_); return local_errors_; }

  std::vector<Goal> goals() const { std::lock_guard<std::mutex> l(mutex_); return goals_; }
  std::size_t goal_count() const { std::lock_guard<std::mutex> l(mutex_); return goals_.size(); }
  std::vector<vda5050_msgs::msg::ActionCommand> commands() const { std::lock_guard<std::mutex> l(mutex_); return commands_; }
  std::vector<vda5050_msgs::msg::Action> executed() const { std::lock_guard<std::mutex> l(mutex_); return executed_; }

  // Active step toward (node_id, sequence_id), optionally of order_id.
  bool stepping_to(const std::string& node_id, uint32_t sequence_id, const std::string& order_id = "") const
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!active_ || !active_->is_active() || active_->is_canceling()) return false;
    const auto& g = *active_->get_goal();
    return g.node.node_id == node_id && g.node.sequence_id == sequence_id && (order_id.empty() || g.order_id == order_id);
  }
  bool has_command(uint8_t command, const std::string& action_id) const
  {
    const auto all = commands();
    return std::any_of(all.begin(), all.end(), [&](const auto& c) { return c.command == command && c.action_id == action_id; });
  }

private:
  void on_accepted(const std::shared_ptr<StepHandle>& handle)
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (active_ && active_->is_active()) {
      auto result = std::make_shared<NavigateToNode::Result>();
      result->outcome = NavigateToNode::Result::PREEMPTED;
      active_->abort(result);
      ++preempted_;
    }
    active_ = handle;
    const auto& g = *handle->get_goal();
    goals_.push_back({g.order_id, g.order_update_id, g.node.node_id, g.node.sequence_id,
                      g.incoming_edge_set ? g.incoming_edge.edge_id : ""});
  }

  void finish_cancels()
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (hold_cancels_ || !active_ || !active_->is_canceling()) return;
    auto result = std::make_shared<NavigateToNode::Result>();
    result->outcome = NavigateToNode::Result::CANCELED;
    active_->canceled(result);
    active_.reset();
    ++cancelled_;
  }

  bool finish(uint8_t outcome, double distance, const std::string& description)
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!active_ || !active_->is_active()) return false;
    auto result = std::make_shared<NavigateToNode::Result>();
    result->outcome = outcome;
    result->distance_driven = distance;
    result->description = description;
    if (outcome == NavigateToNode::Result::REACHED) active_->succeed(result);
    else active_->abort(result);
    active_.reset();
    return true;
  }

  void publish_status()
  {
    vda5050_msgs::msg::DriverStatus m;
    m.session_id = "session-" + std::to_string(session_);
    m.driving = driving_;
    status_pub_->publish(m);
  }

  rclcpp::Node::SharedPtr node_;
  rclcpp_action::Server<NavigateToNode>::SharedPtr server_;
  rclcpp::TimerBase::SharedPtr cancel_timer_;
  rclcpp::Subscription<vda5050_msgs::msg::ActionCommand>::SharedPtr command_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Action>::SharedPtr execute_sub_;
  std::vector<rclcpp::SubscriptionBase::SharedPtr> local_subs_;
  static constexpr std::chrono::milliseconds kStatusLease{300};
  rclcpp::Publisher<vda5050_msgs::msg::DriverStatus>::SharedPtr status_pub_;
  rclcpp::TimerBase::SharedPtr status_timer_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mode_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::ActionState>::SharedPtr feedback_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::AgvPosition>::SharedPtr position_pub_;
  mutable std::mutex mutex_;
  std::shared_ptr<StepHandle> active_;
  std::vector<std::shared_ptr<StepHandle>> lost_;
  bool hold_cancels_{false};
  bool hung_{false};
  bool driving_{false};
  int session_{1};
  std::size_t cancelled_{0};
  std::size_t preempted_{0};
  bool local_driving_{false};
  bool local_paused_{false};
  std::vector<vda5050_msgs::msg::NodeState> local_nodes_;
  std::vector<vda5050_msgs::msg::Error> local_errors_;
  std::vector<Goal> goals_;
  std::vector<vda5050_msgs::msg::ActionCommand> commands_;
  std::vector<vda5050_msgs::msg::Action> executed_;
};

using Command = vda5050_msgs::msg::ActionCommand;

}  // namespace

class LiveCompat : public ::testing::TestWithParam<bool> {
protected:
  void SetUp() override
  {
    const char* broker = std::getenv("VDA5050_TEST_BROKER");
    if (broker == nullptr || *broker == '\0')
    {
      GTEST_SKIP() << "VDA5050_TEST_BROKER not set";
    }
    broker_ = broker;
    static std::atomic<int> counter{0};
    const std::string id = std::to_string(::getpid()) + "_" + std::to_string(++counter);
    serial_ = "compat_" + id;
    ns_ = "/compat_" + id;
    prefix_ = "AMR/v2/" + manufacturer_ + "/" + serial_ + "/";

    bridge_ = std::make_unique<FakeBridge>(ns_);
    executor_ = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    executor_->add_node(bridge_->node());
    spin_thread_ = std::thread([this] { while (!stop_.load()) executor_->spin_some(10ms); });

    master_ = std::make_unique<MasterSide>(broker_, prefix_);
    ASSERT_TRUE(wait_until([&] { return master_->connected(); })) << "master cannot reach " << broker_;
  }

  void TearDown() override
  {
    if (!master_) return;
    // headerId of consecutive state messages must strictly increase on the wire.
    const auto states = master_->states();
    for (std::size_t i = 1; i < states.size(); ++i)
    {
      EXPECT_GT(states[i].value("headerId", 0u), states[i - 1].value("headerId", 0u)) << "state #" << i;
    }
    stop_client();
    stop_.store(true);
    if (spin_thread_.joinable()) spin_thread_.join();
    executor_.reset();
    bridge_.reset();
    master_.reset();
  }

  bool strict() const { return GetParam(); }

  // Start the client adapter node; the bridge and master are already up.
  void start_client()
  {
    rclcpp::NodeOptions options;
    options.use_global_arguments(false);
    options.arguments({"--ros-args", "-r", "__ns:=" + ns_});
    options.parameter_overrides({
      {"mqtt.broker_url", broker_},
      {"vda5050.interface_name", "AMR"},
      {"vda5050.manufacturer", manufacturer_},
      {"vda5050.serial_number", serial_},
      {"vda5050.strict_mode", strict()},
      {"vda5050.state_publish_interval", 1.0},
      {"vda5050.max_finished_instant_actions", int64_t{20}},
      {"factsheet.supported_action_types", std::vector<std::string>{
        "startPause", "stopPause", "cancelOrder", "stateRequest", "factsheetRequest", "initPosition"}},
    });
    client_ = std::make_shared<vda5050_adapter::VDA5050Node>(options);
    executor_->add_node(client_);
    ASSERT_TRUE(wait_until([&] { return bridge_->linked(); })) << "bridge topics not matched";
    ASSERT_TRUE(master_->wait_state([](const json&) { return true; }).has_value()) << "no state from client";
  }

  void stop_client()
  {
    if (!client_) return;
    executor_->remove_node(client_);
    client_.reset();
  }

  // Send full_control's first order (released_count of 3 route points) and wait for its first step.
  std::string start_order(std::size_t released_count = 2)
  {
    const auto order_id = fc::make_uuid();
    master_->send_order(first_order(order_id, manufacturer_, serial_, released_count));
    EXPECT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, order_id); }));
    return order_id;
  }

  // Reach wp0 and wait for the step toward wp1.
  void leave_start()
  {
    ASSERT_TRUE(bridge_->reach(0.0));
    ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp1", 2); }));
  }

  // Enter the incoming edge of the active step, then reach its node.
  void drive_step(double distance = 1.0)
  {
    bridge_->enter();
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(bridge_->reach(distance));
  }

  std::string broker_;
  std::string manufacturer_{"COMPAT"};
  std::string serial_;
  std::string ns_;
  std::string prefix_;
  std::unique_ptr<FakeBridge> bridge_;
  std::unique_ptr<MasterSide> master_;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::shared_ptr<vda5050_adapter::VDA5050Node> client_;
  std::thread spin_thread_;
  std::atomic<bool> stop_{false};
};

TEST_P(LiveCompat, ConnectsWithOnlineFactsheetAndAParsableState)
{
  start_client();
  ASSERT_TRUE(wait_until([&] {
    const auto c = master_->connections();
    return !c.empty() && c.back().value("connectionState", "") == "ONLINE";
  }));
  ASSERT_TRUE(wait_until([&] { return !master_->factsheets().empty(); }));

  const fc::ParsedFactsheet factsheet(master_->factsheets().back());
  EXPECT_TRUE(factsheet.has_content());
  EXPECT_EQ(factsheet.blocking_type_for("cancelOrder", "HARD"), "NONE");
  EXPECT_EQ(factsheet.blocking_type_for("startPause", "NONE"), "NONE");
  EXPECT_TRUE(factsheet.supports_action("factsheetRequest"));
  EXPECT_TRUE(factsheet.supports_scope("initPosition", "INSTANT"));
  ASSERT_TRUE(factsheet.default_state_interval.has_value());
  EXPECT_DOUBLE_EQ(*factsheet.default_state_interval, 1.0);

  const auto state = master_->latest_state();
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(has_required_state_fields(*state)) << state->dump();
  EXPECT_TRUE(fc::ParsedState(*state).operable());
}

TEST_P(LiveCompat, RouteWithHorizonReleaseRunsToCompletion)
{
  start_client();
  const auto order_id = start_order();
  leave_start();
  const auto first_steps = bridge_->goals();
  ASSERT_EQ(first_steps.size(), 2u);
  EXPECT_EQ(first_steps[0].edge_id, "");
  EXPECT_EQ(first_steps[1].edge_id, "e_wp0_wp1");
  EXPECT_EQ(first_steps[1].order_id, order_id);

  bridge_->driving(true);
  drive_step(1.5);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp2", 4); }));

  const auto waiting = master_->wait_state([&](const json& s) {
    return s.value("lastNodeSequenceId", 0u) == 2u && s.value("newBaseRequest", false);
  });
  ASSERT_TRUE(waiting.has_value()) << master_->latest_state()->dump();
  EXPECT_EQ(fc::ParsedState(*waiting).order_id, order_id);
  EXPECT_DOUBLE_EQ(waiting->value("distanceSinceLastNode", 0.0), 1.5);

  const auto before_update = bridge_->goal_count();
  master_->send_order(release_update(order_id, manufacturer_, serial_, 1, 2, 3));
  ASSERT_TRUE(master_->wait_state([](const json& s) {
    return s.value("orderUpdateId", 0u) == 1u && !s.value("newBaseRequest", true);
  }).has_value());
  EXPECT_EQ(bridge_->goal_count(), before_update) << "an update must not resend the step in flight";

  drive_step();
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp3", 6); }));
  EXPECT_EQ(bridge_->goals().back().order_update_id, 1u);
  EXPECT_EQ(bridge_->goals().back().edge_id, "e_wp2_wp3");
  drive_step();
  bridge_->driving(false);

  const auto done = master_->wait_state([&](const json& s) {
    return fc::ParsedState(s).order_finished(order_id, "wp3");
  });
  ASSERT_TRUE(done.has_value()) << master_->latest_state()->dump();
  EXPECT_TRUE(done->at("errors").empty()) << done->dump();
  EXPECT_EQ(bridge_->goal_count(), 4u);
}

TEST_P(LiveCompat, CancelFollowedAtOnceByANewOrderAsFullControlSendsIt)
{
  start_client();
  const auto first_id = start_order();
  leave_start();
  bridge_->driving(true);
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("driving", false); }).has_value());

  const auto cancel = fc::build_cancel_order(10, manufacturer_, serial_, "HARD");
  const auto cancel_id = cancel["actions"][0]["actionId"].get<std::string>();
  const auto second_id = fc::make_uuid();
  fc::CancelTracker tracker;
  tracker.sent(cancel_id, first_id, std::chrono::steady_clock::now());
  master_->send_instant(cancel);
  master_->send_order(first_order(second_id, manufacturer_, serial_, 3));

  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, second_id); }));
  EXPECT_EQ(bridge_->cancelled() + bridge_->preempted(), 1u);
  EXPECT_TRUE(bridge_->commands().empty());
  bridge_->driving(false);

  const auto state = master_->wait_state([&](const json& s) {
    return s.value("orderId", "") == second_id && action_status(s, cancel_id) == "FINISHED";
  });
  ASSERT_TRUE(state.has_value()) << master_->latest_state()->dump();
  EXPECT_FALSE(has_error(*state, "orderError")) << state->dump();

  const auto now = std::chrono::steady_clock::now();
  const auto verdict = tracker.assess(fc::ParsedState(*state), now, now, fc::CancelPolicy{});
  EXPECT_EQ(verdict.outcome, fc::CancelTracker::Outcome::finished);
}

TEST_P(LiveCompat, CancelAloneFinishesWhenTheStepIsCancelledAndTheRobotStops)
{
  start_client();
  const auto order_id = start_order();
  leave_start();
  bridge_->driving(true);
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("driving", false); }).has_value());

  bridge_->hold_cancels(true);
  const auto cancel = fc::build_cancel_order(10, manufacturer_, serial_, "NONE");
  const auto cancel_id = cancel["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(cancel);
  ASSERT_TRUE(wait_until([&] { return bridge_->cancel_pending(); }));
  ASSERT_TRUE(master_->wait_state([&](const json& s) {
    return action_status(s, cancel_id) == "RUNNING";
  }).has_value());

  bridge_->driving(false);
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(action_status(*master_->latest_state(), cancel_id), "RUNNING") << "step goal still in flight";

  bridge_->driving(true);
  bridge_->hold_cancels(false);
  ASSERT_TRUE(wait_until([&] { return bridge_->cancelled() == 1u; }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(action_status(*master_->latest_state(), cancel_id), "RUNNING") << "robot still driving";

  bridge_->driving(false);
  const auto state = master_->wait_state([&](const json& s) {
    return action_status(s, cancel_id) == "FINISHED";
  });
  ASSERT_TRUE(state.has_value()) << master_->latest_state()->dump();
  EXPECT_TRUE(state->at("nodeStates").empty());
  EXPECT_TRUE(state->at("edgeStates").empty());
  EXPECT_EQ(state->value("orderId", "?"), strict() ? order_id : "");
  EXPECT_EQ(bridge_->goal_count(), 2u);

  fc::CancelTracker tracker;
  const auto sent_at = std::chrono::steady_clock::now();
  tracker.sent(cancel_id, order_id, sent_at);
  const auto verdict = tracker.assess(fc::ParsedState(*state), sent_at, sent_at, fc::CancelPolicy{});
  EXPECT_EQ(verdict.outcome, fc::CancelTracker::Outcome::finished);
}

TEST_P(LiveCompat, RestartedFleetAdapterCancelsTheOrderItDidNotSendThenSendsItsOwn)
{
  start_client();
  const auto left_over = start_order();
  leave_start();
  bridge_->driving(true);
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("driving", false); }).has_value());

  const auto cancel = fc::build_instant_action(30, manufacturer_, serial_, "cancelOrder", {{"orderId", left_over}}, "HARD");
  const auto own_id = fc::make_uuid();
  master_->send_instant(cancel.message);
  master_->send_order(first_order(own_id, manufacturer_, serial_, 3));

  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, own_id); }));
  const auto state = master_->wait_state([&](const json& s) { return s.value("orderId", "") == own_id; });
  ASSERT_TRUE(state.has_value()) << master_->latest_state()->dump();
  EXPECT_FALSE(has_error(*state, "orderError")) << state->dump();
  EXPECT_FALSE(has_error(*state, "noOrderToCancel")) << state->dump();
}

TEST_P(LiveCompat, NewOrderWhileSteppingPreemptsOnlyInDefaultMode)
{
  start_client();
  const auto first_id = start_order();
  leave_start();
  bridge_->driving(true);

  const auto second_id = fc::make_uuid();
  master_->send_order(first_order(second_id, manufacturer_, serial_, 3));
  if (strict()) {
    ASSERT_TRUE(master_->wait_state([](const json& s) { return has_error(s, "orderError"); }).has_value());
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(bridge_->stepping_to("wp1", 2, first_id));
    EXPECT_EQ(bridge_->preempted(), 0u);
    return;
  }
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, second_id); }));
  EXPECT_EQ(bridge_->preempted(), 1u);
  EXPECT_EQ(bridge_->cancelled(), 0u);
}

TEST_P(LiveCompat, CancelWithoutAnOrderFailsWithNoOrderToCancel)
{
  start_client();
  const auto cancel = fc::build_cancel_order(10, manufacturer_, serial_, "NONE");
  const auto cancel_id = cancel["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(cancel);
  const auto state = master_->wait_state([&](const json& s) { return action_status(s, cancel_id) == "FAILED"; });
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(has_error(*state, "noOrderToCancel"));
  EXPECT_EQ(bridge_->goal_count(), 0u);
  EXPECT_EQ(bridge_->cancelled(), 0u);
}

TEST_P(LiveCompat, PauseStopsTheStepAndResumeSendsItAgain)
{
  start_client();
  start_order();
  leave_start();
  bridge_->driving(true);
  bridge_->enter();

  bridge_->hold_cancels(true);
  const auto pause = fc::build_start_pause(11, manufacturer_, serial_);
  const auto pause_id = pause["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(pause);
  ASSERT_TRUE(wait_until([&] { return bridge_->cancel_pending(); }));
  ASSERT_TRUE(master_->wait_state([&](const json& s) { return action_status(s, pause_id) == "RUNNING"; }).has_value());

  bridge_->hold_cancels(false);
  bridge_->driving(false);
  const auto paused = master_->wait_state([&](const json& s) { return action_status(s, pause_id) == "FINISHED"; });
  ASSERT_TRUE(paused.has_value());
  EXPECT_TRUE(fc::ParsedState(*paused).paused);
  EXPECT_EQ(paused->at("edgeStates").size(), 3u);

  const auto before_resume = bridge_->goal_count();
  const auto resume = fc::build_stop_pause(12, manufacturer_, serial_);
  const auto resume_id = resume["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(resume);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp1", 2); }));
  EXPECT_EQ(bridge_->goal_count(), before_resume + 1);
  bridge_->driving(true);
  const auto resumed = master_->wait_state([&](const json& s) {
    return action_status(s, resume_id) == "FINISHED" && s.value("driving", false);
  });
  ASSERT_TRUE(resumed.has_value());
  EXPECT_FALSE(fc::ParsedState(*resumed).paused);

  drive_step();
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("lastNodeId", "") == "wp1"; }).has_value());
}

TEST_P(LiveCompat, FailureOfAStepBeingCancelledForAPauseKeepsTheOrder)
{
  start_client();
  const auto order_id = start_order();
  leave_start();
  bridge_->hold_cancels(true);
  master_->send_instant(fc::build_start_pause(11, manufacturer_, serial_));
  ASSERT_TRUE(wait_until([&] { return bridge_->cancel_pending(); }));
  ASSERT_TRUE(bridge_->fail("Nav2 kept failing"));

  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("paused", false); }).has_value());
  std::this_thread::sleep_for(200ms);
  const auto state = master_->latest_state();
  EXPECT_FALSE(has_error(*state, "navigationError")) << state->dump();
  EXPECT_EQ(state->at("nodeStates").size(), 3u);

  bridge_->hold_cancels(false);
  master_->send_instant(fc::build_stop_pause(12, manufacturer_, serial_));
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp1", 2, order_id); }));
}

TEST_P(LiveCompat, StepReachedWithoutEdgeFeedbackStillCompletesItsEdge)
{
  start_client();
  const auto order_id = fc::make_uuid();
  auto order = first_order(order_id, manufacturer_, serial_, 3);
  order["edges"][0]["actions"] = json::array({fc::make_action("beep", "NONE", "beep-1")});
  master_->send_order(order);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, order_id); }));
  leave_start();
  ASSERT_TRUE(bridge_->reach());

  const auto state = master_->wait_state([](const json& s) { return s.value("lastNodeId", "") == "wp1"; });
  ASSERT_TRUE(state.has_value());
  std::vector<std::string> edges;
  for (const auto& e : state->at("edgeStates")) edges.push_back(e.value("edgeId", ""));
  EXPECT_EQ(edges, (std::vector<std::string>{"e_wp1_wp2", "e_wp2_wp3"}));
  ASSERT_TRUE(wait_until([&] {
    const auto e = bridge_->executed();
    return !e.empty() && e.back().action_id == "beep-1";
  })) << "edge action runs even when the edge is only reported at the node";
}

TEST_P(LiveCompat, PauseWithoutAnOrderFinishesAtOnceAndHoldsTheNextOrder)
{
  start_client();
  const auto pause = fc::build_start_pause(11, manufacturer_, serial_);
  const auto pause_id = pause["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(pause);
  ASSERT_TRUE(master_->wait_state([&](const json& s) {
    return action_status(s, pause_id) == "FINISHED" && s.value("paused", false);
  }).has_value());

  const auto order_id = fc::make_uuid();
  master_->send_order(first_order(order_id, manufacturer_, serial_, 3));
  ASSERT_TRUE(master_->wait_state([&](const json& s) { return s.value("orderId", "") == order_id; }).has_value());
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(bridge_->goal_count(), 0u);

  master_->send_instant(fc::build_stop_pause(12, manufacturer_, serial_));
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, order_id); }));
}

TEST_P(LiveCompat, EdgeActionStillRunningAtTheEdgeEndIsCancelledNotTheOrder)
{
  start_client();
  const auto order_id = fc::make_uuid();
  auto order = first_order(order_id, manufacturer_, serial_, 3);
  order["edges"][0]["actions"] = json::array({fc::make_action("beep", "NONE", "beep-1")});
  master_->send_order(order);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, order_id); }));
  leave_start();

  bridge_->driving(true);
  bridge_->enter();
  ASSERT_TRUE(wait_until([&] {
    const auto e = bridge_->executed();
    return !e.empty() && e.back().action_id == "beep-1";
  }));
  bridge_->feedback("beep-1", "RUNNING");
  std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(bridge_->reach());

  ASSERT_TRUE(wait_until([&] { return bridge_->has_command(Command::CANCEL, "beep-1"); }));
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp2", 4); }));
  EXPECT_EQ(bridge_->cancelled(), 0u);
  const auto state = master_->wait_state([](const json& s) { return action_status(s, "beep-1") == "FAILED"; });
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->at("nodeStates").empty());
}

TEST_P(LiveCompat, BlockingNodeActionHoldsTheRouteOnlyInStrictMode)
{
  start_client();
  const auto order_id = fc::make_uuid();
  auto order = first_order(order_id, manufacturer_, serial_, 3);
  order["nodes"][1]["actions"] = json::array({fc::make_action("pick", "HARD", "pick-1")});
  master_->send_order(order);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, order_id); }));
  leave_start();
  drive_step();

  ASSERT_TRUE(wait_until([&] {
    const auto e = bridge_->executed();
    return !e.empty() && e.back().action_id == "pick-1";
  }));
  bridge_->feedback("pick-1", "RUNNING");
  std::this_thread::sleep_for(200ms);
  if (strict()) {
    EXPECT_FALSE(bridge_->stepping_to("wp2", 4)) << "HARD action running";
    bridge_->feedback("pick-1", "FINISHED");
  }
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp2", 4); }));
}

TEST_P(LiveCompat, MalformedOrderIsReportedAndNotDriven)
{
  start_client();
  const auto order_id = fc::make_uuid();
  auto order = first_order(order_id, manufacturer_, serial_, 3);
  order["nodes"][1]["actions"] = json::array({{{"actionType", "pick"}, {"actionId", "p1"}, {"blockingType", "hard"}}});
  master_->send_order(order);

  const auto state = master_->wait_state([](const json& s) { return has_error(s, "validationError"); });
  ASSERT_TRUE(state.has_value());
  const auto& error = *std::find_if(state->at("errors").begin(), state->at("errors").end(),
                                    [](const json& e) { return e.value("errorType", "") == "validationError"; });
  EXPECT_NE(error.dump().find(order_id), std::string::npos);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(bridge_->goal_count(), 0u);

  master_->send_raw_order("{not json");
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(bridge_->goal_count(), 0u);

  start_order();
  ASSERT_TRUE(master_->wait_state([](const json& s) { return !has_error(s, "validationError"); }).has_value());
}

TEST_P(LiveCompat, ResentOrderIsNotDrivenTwice)
{
  start_client();
  const auto order_id = fc::make_uuid();
  const auto order = first_order(order_id, manufacturer_, serial_, 3);
  master_->send_order(order);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp0", 0, order_id); }));
  master_->send_order(order);
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(bridge_->goal_count(), 1u);

  const auto state = master_->latest_state();
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(state->at("errors").empty()) << state->dump();
}

TEST_P(LiveCompat, RestartedDriverGetsTheActiveStepAgain)
{
  start_client();
  const auto order_id = start_order();
  leave_start();
  bridge_->driving(true);
  bridge_->enter();
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("driving", false); }).has_value());

  const auto before = bridge_->goal_count();
  bridge_->restart();
  ASSERT_TRUE(wait_until([&] { return bridge_->goal_count() == before + 1; }));
  const auto resent = bridge_->goals().back();
  EXPECT_EQ(resent.order_id, order_id);
  EXPECT_EQ(resent.node_id, "wp1");
  EXPECT_EQ(resent.edge_id, "e_wp0_wp1");
  ASSERT_TRUE(master_->wait_state([](const json& s) { return !s.value("driving", true); }).has_value());

  drive_step();
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp2", 4); }));
  const auto state = master_->wait_state([](const json& s) { return s.value("lastNodeId", "") == "wp1"; });
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(state->at("errors").empty()) << state->dump();
}

TEST_P(LiveCompat, StepEndedWithoutAResultIsNotCountedAsReached)
{
  start_client();
  start_order();
  leave_start();
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("lastNodeId", "") == "wp0"; }).has_value());

  const auto before = bridge_->goal_count();
  bridge_->abandon();
  ASSERT_TRUE(wait_until([&] { return bridge_->goal_count() == before + 1; }));
  EXPECT_TRUE(wait_until([&] { return bridge_->stepping_to("wp1", 2); }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(master_->latest_state()->value("lastNodeId", ""), "wp0");
  EXPECT_EQ(master_->latest_state()->at("nodeStates").size(), 3u);
}

TEST_P(LiveCompat, RestartedDriverCompletesAPendingPause)
{
  start_client();
  start_order();
  leave_start();
  bridge_->hold_cancels(true);
  const auto pause = fc::build_start_pause(11, manufacturer_, serial_);
  const auto pause_id = pause["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(pause);
  ASSERT_TRUE(wait_until([&] { return bridge_->cancel_pending(); }));

  const auto before = bridge_->goal_count();
  bridge_->restart();
  ASSERT_TRUE(master_->wait_state([&](const json& s) { return action_status(s, pause_id) == "FINISHED"; }).has_value());
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(bridge_->goal_count(), before) << "paused: no step to resend";
}

TEST_P(LiveCompat, LostDriverDropsTheStepWithAFatalErrorUntilItIsBack)
{
  if (std::string(rmw_get_implementation_identifier()) == "rmw_fastrtps_cpp") {
    GTEST_SKIP() << "Fast DDS reports no liveliness loss of a writer in the same process";
  }
  start_client();
  start_order();
  leave_start();
  bridge_->driving(true);
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("driving", false); }).has_value());

  const auto goals_before_loss = bridge_->goal_count();
  bridge_->hold_cancels(true);
  bridge_->hang(true);
  const auto lost = master_->wait_state([](const json& s) {
    return has_error(s, "driverConnectionError") && !s.value("driving", true);
  });
  ASSERT_TRUE(lost.has_value()) << master_->latest_state()->dump();
  for (const auto& e : lost->at("errors")) {
    if (e.value("errorType", "") == "driverConnectionError") {
      EXPECT_EQ(e.value("errorLevel", ""), "FATAL");
    }
  }
  EXPECT_EQ(lost->at("nodeStates").size(), 3u);
  EXPECT_EQ(fc::ParsedState(*lost).first_fatal_error(), "driverConnectionError") << "generic FATAL for any master";
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(bridge_->goal_count(), goals_before_loss) << "no step to a lost driver";

  const auto pause = fc::build_start_pause(11, manufacturer_, serial_);
  const auto pause_id = pause["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(pause);
  ASSERT_TRUE(master_->wait_state([&](const json& s) {
    return action_status(s, pause_id) == "FINISHED" && s.value("paused", false);
  }).has_value()) << "pause must not wait for the lost goal";

  const auto before = bridge_->goal_count();
  bridge_->hold_cancels(false);
  bridge_->driving(false);
  bridge_->hang(false);
  ASSERT_TRUE(master_->wait_state([](const json& s) { return !has_error(s, "driverConnectionError"); }).has_value());
  EXPECT_EQ(bridge_->goal_count(), before) << "still paused";

  master_->send_instant(fc::build_stop_pause(12, manufacturer_, serial_));
  ASSERT_TRUE(wait_until([&] { return bridge_->goal_count() == before + 1 && bridge_->stepping_to("wp1", 2); }));
}

TEST_P(LiveCompat, GoalsLeftByAPreviousAdapterAreCancelledBeforeTheFirstStep)
{
  auto previous = rclcpp_action::create_client<FakeBridge::NavigateToNode>(
    bridge_->node(), ns_ + "/vda5050_client_adapter/navigate_to_node");
  ASSERT_TRUE(previous->wait_for_action_server(2s));
  FakeBridge::NavigateToNode::Goal goal;
  goal.order_id = "previous";
  goal.node.node_id = "wp9";
  previous->async_send_goal(goal);
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp9", 0, "previous"); }));

  start_client();
  ASSERT_TRUE(wait_until([&] { return bridge_->cancelled() == 1u; }));
  const auto order_id = start_order();
  EXPECT_EQ(bridge_->goals().back().order_id, order_id);
  EXPECT_EQ(bridge_->cancelled(), 1u);
}

TEST_P(LiveCompat, OperatingModeLatchedBeforeTheClientStartsIsReported)
{
  bridge_->operating_mode("MANUAL");
  std::this_thread::sleep_for(100ms);
  start_client();
  const auto state = master_->wait_state([](const json& s) { return s.value("operatingMode", "") == "MANUAL"; });
  ASSERT_TRUE(state.has_value()) << master_->latest_state()->dump();
  EXPECT_FALSE(fc::ParsedState(*state).operable());

  bridge_->operating_mode("AUTOMATIC");
  ASSERT_TRUE(master_->wait_state([](const json& s) { return fc::ParsedState(s).operable(); }).has_value());
}

TEST_P(LiveCompat, FactsheetRequestRepublishesTheFactsheet)
{
  start_client();
  ASSERT_TRUE(wait_until([&] { return !master_->factsheets().empty(); }));
  const auto first_header = master_->factsheets().back().value("headerId", 0u);

  const auto request = fc::build_instant_action(20, manufacturer_, serial_, "factsheetRequest", json::object(), "NONE");
  master_->send_instant(request.message);

  ASSERT_TRUE(wait_until([&] { return master_->factsheets().back().value("headerId", 0u) > first_header; }));
  ASSERT_TRUE(master_->wait_state([&](const json& s) {
    return action_status(s, request.action_id) == "FINISHED";
  }).has_value());
  EXPECT_TRUE(bridge_->executed().empty());
}

TEST_P(LiveCompat, FailedStepIsReportedOnceAndDropsTheOrder)
{
  start_client();
  for (int round = 0; round < 2; ++round)
  {
    start_order(3);
    leave_start();
    const auto before = master_->state_count();
    ASSERT_TRUE(bridge_->fail("Dispatch did not become possible"));

    ASSERT_TRUE(wait_until([&] {
      const auto states = master_->states();
      return std::any_of(states.begin() + static_cast<std::ptrdiff_t>(before), states.end(), [](const json& s) {
        return has_error(s, "navigationError") && !s.at("nodeStates").empty();
      });
    })) << "round " << round;
    const auto settled = master_->wait_state([](const json& s) {
      return !has_error(s, "navigationError") && s.at("nodeStates").empty();
    });
    ASSERT_TRUE(settled.has_value()) << "round " << round << ": " << master_->latest_state()->dump();
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(has_error(*master_->latest_state(), "navigationError"));
    EXPECT_FALSE(bridge_->stepping_to("wp1", 2));
  }
}

TEST_P(LiveCompat, DroppedStepClearsTheOrderWithoutAnError)
{
  start_client();
  const auto order_id = start_order(3);
  leave_start();
  const auto before = master_->state_count();
  ASSERT_TRUE(bridge_->drop("cancelled on the robot"));

  const auto state = master_->wait_state([](const json& s) { return s.at("nodeStates").empty(); });
  ASSERT_TRUE(state.has_value());
  const auto states = master_->states();
  EXPECT_TRUE(std::none_of(states.begin() + static_cast<std::ptrdiff_t>(before), states.end(),
                           [](const json& s) { return has_error(s, "navigationError"); }));
  EXPECT_EQ(state->value("orderId", "?"), strict() ? order_id : "");

  const auto next = start_order();
  EXPECT_NE(next, order_id);
}

TEST_P(LiveCompat, LocalStatusTopicsFollowTheRoute)
{
  start_client();
  const auto order_id = start_order(3);
  leave_start();
  bridge_->driving(true);
  ASSERT_TRUE(wait_until([&] { return bridge_->local_driving(); }));
  drive_step(1.25);
  ASSERT_TRUE(wait_until([&] { return bridge_->local_nodes().size() == 2u; }));
  const auto reached = bridge_->local_nodes().back();
  EXPECT_EQ(reached.node_id, "wp1");
  EXPECT_EQ(reached.sequence_id, 2u);
  EXPECT_DOUBLE_EQ(reached.distance_driven, 1.25);
  EXPECT_TRUE(reached.node_position_set);

  master_->send_instant(fc::build_start_pause(11, manufacturer_, serial_));
  ASSERT_TRUE(wait_until([&] { return bridge_->local_paused(); }));
  master_->send_instant(fc::build_stop_pause(12, manufacturer_, serial_));
  ASSERT_TRUE(wait_until([&] { return !bridge_->local_paused() && bridge_->stepping_to("wp2", 4); }));

  ASSERT_TRUE(bridge_->fail("Nav2 kept failing"));
  ASSERT_TRUE(wait_until([&] { return bridge_->local_errors().size() == 1u; }));
  const auto error = bridge_->local_errors().front();
  EXPECT_EQ(error.error_type, "navigationError");
  EXPECT_EQ(error.error_level, "FATAL");
  ASSERT_FALSE(error.error_references.empty());
  EXPECT_EQ(error.error_references.front().reference_value, order_id);

  ASSERT_TRUE(master_->wait_state([](const json& s) {
    return !has_error(s, "navigationError") && s.at("nodeStates").empty();
  }).has_value());
  std::this_thread::sleep_for(300ms);
  EXPECT_FALSE(has_error(*master_->latest_state(), "navigationError")) << "own error copy must not come back";
}

TEST_P(LiveCompat, AnInstantActionBurstPublishesFewStates)
{
  start_client();
  start_order();
  ASSERT_TRUE(bridge_->reach(0.0));
  ASSERT_TRUE(wait_until([&] { return bridge_->stepping_to("wp1", 2); }));
  std::this_thread::sleep_for(200ms);

  const auto before = master_->state_count();
  const auto cancel = fc::build_cancel_order(10, manufacturer_, serial_, "NONE");
  master_->send_instant(cancel);
  std::this_thread::sleep_for(400ms);
  const auto published = master_->state_count() - before;
  EXPECT_GE(published, 1u);
  EXPECT_LE(published, 2u);
}

TEST_P(LiveCompat, ConcurrentMqttAndRosTrafficStaysConsistent)
{
  start_client();
  start_order();
  bridge_->driving(true);

  std::atomic<bool> done{false};
  std::thread ros_traffic([&] {
    int i = 0;
    while (!done.load())
    {
      bridge_->position(0.001 * i++);
      if (i % 50 == 0) bridge_->reach();
      if (i % 70 == 0) bridge_->enter();
      std::this_thread::sleep_for(1ms);
    }
  });
  for (int i = 0; i < 300; ++i)
  {
    master_->send_instant(fc::build_state_request(100 + i, manufacturer_, serial_));
    if (i % 100 == 0) master_->send_order(first_order(fc::make_uuid(), manufacturer_, serial_, 3));
  }
  std::this_thread::sleep_for(500ms);
  done.store(true);
  ros_traffic.join();

  const auto last = fc::build_state_request(999, manufacturer_, serial_);
  const auto last_id = last["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(last);
  const auto state = master_->wait_state([&](const json& s) { return action_status(s, last_id) == "FINISHED"; });
  ASSERT_TRUE(state.has_value());
  EXPECT_LE(state->at("actionStates").size(), 22u);
  EXPECT_TRUE(has_required_state_fields(*state));
  EXPECT_TRUE(std::none_of(state->at("errors").begin(), state->at("errors").end(), [](const json& e) {
    return e.value("errorType", "") == "navigationError";
  })) << state->dump();
}

INSTANTIATE_TEST_SUITE_P(Modes, LiveCompat, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Strict" : "Default"; });
