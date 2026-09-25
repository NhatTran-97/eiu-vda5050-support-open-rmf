/**
 * @file test_system_e2e.cpp
 * @brief Fleet adapter (MQTT) -> vda5050_client_adapter -> tb3_vda5050_bridge -> fake Nav2.
 *
 * Messages are built and parsed with vda5050_fleet_adapter_full_control's own code; the client
 * adapter and the bridge are the real nodes. Needs VDA5050_TEST_BROKER=tcp://host:port.
 *
 * Coverage (client in default and strict mode):
 *  - route with horizon release driven to completion
 *  - cancelOrder followed at once by a new order, repeated
 *  - pause held across cancel + new order (traffic hold), then stopPause
 *  - bridge restart in the middle of a route after an order update: the client resends the step
 *  - bridge gone: driverConnectionError until it is back, then the route finishes
 */

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <rmw/rmw.h>

#include "tb3_vda5050_bridge/bridge_node.hpp"
#include "vda5050_client_adapter/mqtt_client.hpp"
#include "vda5050_client_adapter/vda5050_node.hpp"

#include "vda5050_fleet_adapter_full_control/vda5050/cancel_tracker.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

namespace fc = vda5050_fleet_adapter_full_control::vda5050;
using json = nlohmann::json;
using namespace std::chrono_literals;
using NavigateToPose = nav2_msgs::action::NavigateToPose;

namespace {

class RosEnvironment : public ::testing::Environment {
public:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};
const auto* const kRosEnvironment = ::testing::AddGlobalTestEnvironment(new RosEnvironment);

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 8s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

rclcpp::NodeOptions node_options(const std::string& ns, std::vector<rclcpp::Parameter> params = {})
{
  rclcpp::NodeOptions options;
  options.use_global_arguments(false);
  options.arguments({"--ros-args", "-r", "__ns:=" + ns});
  options.parameter_overrides(std::move(params));
  return options;
}

// Route wp1..wp3 at x = 1, 2, 3 from base wp0 at the robot (x = 0).
std::vector<fc::RouteWaypoint> route()
{
  std::vector<fc::RouteWaypoint> points;
  for (int i = 1; i <= 3; ++i) points.push_back({"wp" + std::to_string(i), {1.0 * i, 0.0, 0.0}, std::nullopt});
  return points;
}

std::optional<std::string> action_status(const json& state, const std::string& action_id)
{
  for (const auto& a : state["actionStates"])
  {
    if (a.value("actionId", "") == action_id) return a.value("actionStatus", "");
  }
  return std::nullopt;
}

// NavigateToPose server: accepts goals, finishes them on request, honours cancel requests.
class FakeNav2 {
public:
  using GoalHandle = rclcpp_action::ServerGoalHandle<NavigateToPose>;

  FakeNav2(rclcpp::Node::SharedPtr node, const std::string& name) : node_(std::move(node))
  {
    server_ = rclcpp_action::create_server<NavigateToPose>(
      node_, name,
      [this](const rclcpp_action::GoalUUID&, std::shared_ptr<const NavigateToPose::Goal> goal) {
        std::lock_guard<std::mutex> l(mutex_);
        goals_.push_back(goal->pose.pose.position.x);
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
      },
      [](const std::shared_ptr<GoalHandle>&) { return rclcpp_action::CancelResponse::ACCEPT; },
      [this](const std::shared_ptr<GoalHandle>& handle) {
        std::lock_guard<std::mutex> l(mutex_);
        if (active_ && active_->is_active()) active_->abort(std::make_shared<NavigateToPose::Result>());
        active_ = handle;
      });
    cancel_timer_ = node_->create_wall_timer(10ms, [this] {
      std::lock_guard<std::mutex> l(mutex_);
      if (active_ && active_->is_canceling()) active_->canceled(std::make_shared<NavigateToPose::Result>());
    });
  }

  std::vector<double> goals() const { std::lock_guard<std::mutex> l(mutex_); return goals_; }

  // Target x of the goal being executed, if any.
  std::optional<double> active_goal() const
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!active_ || !active_->is_active() || active_->is_canceling()) return std::nullopt;
    return active_->get_goal()->pose.pose.position.x;
  }

  void succeed()
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (active_ && active_->is_active()) active_->succeed(std::make_shared<NavigateToPose::Result>());
  }

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp_action::Server<NavigateToPose>::SharedPtr server_;
  rclcpp::TimerBase::SharedPtr cancel_timer_;
  mutable std::mutex mutex_;
  std::vector<double> goals_;
  std::shared_ptr<GoalHandle> active_;
};

// Fleet adapter side on MQTT.
class Master {
public:
  Master(const std::string& broker, const std::string& prefix) : prefix_(prefix)
  {
    vda5050_adapter::MqttConfig cfg;
    cfg.broker_url = broker;
    cfg.client_id = "e2e_master_" + std::to_string(::getpid()) + "_" + std::to_string(++instances_);
    cfg.clean_session = true;
    client_ = std::make_unique<vda5050_adapter::MqttClient>(cfg);
    client_->subscribe(prefix_ + "state", 0, [this](const auto& m) {
      auto j = json::parse(m.payload, nullptr, false);
      std::lock_guard<std::mutex> l(mutex_);
      last_state_ = std::move(j);
    });
    client_->connect();
  }
  ~Master() { client_.reset(); }

  bool connected() const { return client_->is_connected(); }
  void send_order(const json& o) { client_->publish(prefix_ + "order", o.dump(), 0, false); }
  void send_instant(const json& ia) { client_->publish(prefix_ + "instantActions", ia.dump(), 0, false); }

  std::optional<json> state() const
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (last_state_.is_null()) return std::nullopt;
    return std::optional<json>(std::in_place, last_state_);
  }

  template <typename Predicate>
  std::optional<json> wait_state(Predicate predicate, std::chrono::milliseconds timeout = 8s)
  {
    std::optional<json> match;
    wait_until([&] {
      const auto s = state();
      if (s && predicate(*s)) { match = s; return true; }
      return false;
    }, timeout);
    return match;
  }

private:
  static inline std::atomic<int> instances_{0};
  std::string prefix_;
  std::unique_ptr<vda5050_adapter::MqttClient> client_;
  mutable std::mutex mutex_;
  json last_state_;
};

}  // namespace

class SystemE2E : public ::testing::TestWithParam<bool> {
protected:
  void SetUp() override
  {
    const char* broker = std::getenv("VDA5050_TEST_BROKER");
    if (broker == nullptr || *broker == '\0') GTEST_SKIP() << "VDA5050_TEST_BROKER not set";

    static std::atomic<int> counter{0};
    const std::string id = std::to_string(::getpid()) + "_" + std::to_string(++counter);
    ns_ = "/e2e_" + id;
    serial_ = "e2e_" + id;

    sim_ = std::make_shared<rclcpp::Node>("sim", node_options(ns_));
    nav2_ = std::make_unique<FakeNav2>(sim_, ns_ + "/navigate_to_pose");
    amcl_pub_ = sim_->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
      ns_ + "/amcl_pose", rclcpp::QoS(10).transient_local());
    odom_pub_ = sim_->create_publisher<nav_msgs::msg::Odometry>(ns_ + "/odom", rclcpp::QoS(10));
    geometry_msgs::msg::PoseWithCovarianceStamped pose;
    pose.header.frame_id = "map";
    pose.pose.pose.orientation.w = 1.0;
    pose.pose.covariance[0] = 0.01;
    pose.pose.covariance[7] = 0.01;
    amcl_pub_->publish(pose);

    client_ = std::make_shared<vda5050_adapter::VDA5050Node>(node_options(ns_, {
      {"mqtt.broker_url", std::string(broker)},
      {"vda5050.interface_name", "AMR"},
      {"vda5050.manufacturer", manufacturer_},
      {"vda5050.serial_number", serial_},
      {"vda5050.state_publish_interval", 1.0},
      {"vda5050.strict_mode", GetParam()},
    }));
    bridge_ = std::make_shared<tb3_vda5050_bridge::BridgeNode>(node_options(ns_, {
      {"adapter_ns", ns_ + "/vda5050_client_adapter"},
      {"odom_topic", ns_ + "/odom"},
      {"amcl_pose_topic", ns_ + "/amcl_pose"},
      {"initial_pose_topic", ns_ + "/initialpose"},
      {"battery_topic", ns_ + "/battery"},
      {"speed_limit_topic", ns_ + "/speed_limit"},
      {"diagnostics_topic", ns_ + "/diagnostics"},
      {"nav2_action_name", ns_ + "/navigate_to_pose"},
      {"map_id", "map"},
      {"nav2_retry_period_sec", 0.2},
      {"driver_status_lease_sec", 0.3},
    }));

    executor_ = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    executor_->add_node(sim_);
    executor_->add_node(client_);
    executor_->add_node(bridge_);
    spin_thread_ = std::thread([this] { while (!stop_.load()) executor_->spin_some(5ms); });

    master_ = std::make_unique<Master>(broker, "AMR/v2/" + manufacturer_ + "/" + serial_ + "/");
    ASSERT_TRUE(wait_until([&] { return master_->connected(); }));
    ASSERT_TRUE(master_->wait_state([](const json& s) {
      return fc::ParsedState(s).has_position();
    }).has_value()) << "no localized state from the client";
  }

  void TearDown() override
  {
    master_.reset();
    stop_.store(true);
    if (spin_thread_.joinable()) spin_thread_.join();
    executor_.reset();
    bridge_.reset();
    client_.reset();
    nav2_.reset();
    sim_.reset();
  }

  // full_control's order for the route from wp0, `released` route points released.
  json order(const std::string& order_id, int update_id = 0, std::size_t released = 3, std::size_t stitch = 0)
  {
    return fc::build_route_order(1 + update_id, order_id, manufacturer_, serial_, "wp0", {0.0, 0.0, 0.0},
                                 route(), "map", update_id, released, stitch);
  }

  bool nav2_heads_to(double x) { return wait_until([&] { return nav2_->active_goal() == x; }); }

  // Destroy and recreate the bridge node with the same parameters.
  void restart_bridge()
  {
    executor_->remove_node(bridge_);
    const auto options = bridge_->get_node_options();
    bridge_.reset();
    bridge_ = std::make_shared<tb3_vda5050_bridge::BridgeNode>(options);
    executor_->add_node(bridge_);
  }

  std::string ns_;
  std::string serial_;
  std::string manufacturer_{"E2E"};
  rclcpp::Node::SharedPtr sim_;
  std::unique_ptr<FakeNav2> nav2_;
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr amcl_pub_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odom_pub_;
  std::shared_ptr<vda5050_adapter::VDA5050Node> client_;
  std::shared_ptr<tb3_vda5050_bridge::BridgeNode> bridge_;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::unique_ptr<Master> master_;
  std::thread spin_thread_;
  std::atomic<bool> stop_{false};
};

TEST_P(SystemE2E, RouteWithHorizonReleaseIsDrivenToCompletion)
{
  const auto order_id = fc::make_uuid();
  master_->send_order(order(order_id, 0, 2));
  ASSERT_TRUE(nav2_heads_to(1.0));
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("driving", false); }).has_value());
  nav2_->succeed();
  ASSERT_TRUE(nav2_heads_to(2.0));

  const auto waiting = master_->wait_state([](const json& s) {
    return s.value("lastNodeSequenceId", 0u) == 2u && s.value("newBaseRequest", false);
  });
  ASSERT_TRUE(waiting.has_value()) << master_->state()->dump();

  master_->send_order(order(order_id, 1, 3, 2));
  ASSERT_TRUE(master_->wait_state([](const json& s) { return !s.value("newBaseRequest", true); }).has_value());
  nav2_->succeed();
  ASSERT_TRUE(nav2_heads_to(3.0));
  nav2_->succeed();

  const auto done = master_->wait_state([&](const json& s) {
    return fc::ParsedState(s).order_finished(order_id, "wp3");
  });
  ASSERT_TRUE(done.has_value()) << master_->state()->dump();
  EXPECT_TRUE(done->at("errors").empty()) << done->dump();
}

TEST_P(SystemE2E, CancelFollowedAtOnceByANewOrderKeepsTheNewOrder)
{
  std::string running;
  for (int round = 0; round < 10; ++round)
  {
    // A new orderId follows a cancelOrder of the running order, as the fleet adapter sends it.
    if (!running.empty())
    {
      master_->send_instant(fc::build_instant_action(200 + round, manufacturer_, serial_, "cancelOrder",
                                                     {{"orderId", running}}, "NONE").message);
    }
    const auto first_id = fc::make_uuid();
    master_->send_order(order(first_id));
    ASSERT_TRUE(master_->wait_state([&](const json& s) {
      return s.value("orderId", "") == first_id && s.value("driving", false);
    }).has_value()) << "round " << round;

    const auto cancel = fc::build_cancel_order(100 + round, manufacturer_, serial_, "HARD");
    const auto cancel_id = cancel["actions"][0]["actionId"].get<std::string>();
    const auto second_id = fc::make_uuid();
    fc::CancelTracker tracker;
    const auto sent_at = std::chrono::steady_clock::now();
    tracker.sent(cancel_id, first_id, sent_at);
    master_->send_instant(cancel);
    master_->send_order(order(second_id));

    const auto state = master_->wait_state([&](const json& s) {
      const auto cancel_status = action_status(s, cancel_id);
      return s.value("orderId", "") == second_id && s.value("driving", false) &&
             (!cancel_status.has_value() || *cancel_status == "FINISHED");
    });
    ASSERT_TRUE(state.has_value()) << "round " << round << ": " << master_->state()->dump();
    std::this_thread::sleep_for(300ms);
    const auto settled = master_->state();
    ASSERT_TRUE(settled.has_value());
    EXPECT_EQ(settled->value("orderId", ""), second_id) << "round " << round;
    EXPECT_FALSE(settled->at("nodeStates").empty()) << "round " << round << ": " << settled->dump();
    EXPECT_TRUE(settled->at("errors").empty()) << "round " << round << ": " << settled->dump();
    EXPECT_TRUE(nav2_->active_goal().has_value()) << "round " << round;
    // cancelOrder FINISHED, or removed with the old order's finished instant actions.
    const auto now = std::chrono::steady_clock::now();
    const auto outcome = tracker.assess(fc::ParsedState(*settled), now, now, fc::CancelPolicy{}).outcome;
    EXPECT_TRUE(outcome == fc::CancelTracker::Outcome::finished ||
                outcome == fc::CancelTracker::Outcome::order_gone) << "round " << round;
    running = second_id;
  }
}

TEST_P(SystemE2E, PauseHoldsAcrossCancelAndNewOrderUntilStopPause)
{
  master_->send_order(order(fc::make_uuid()));
  ASSERT_TRUE(nav2_heads_to(1.0));

  const auto pause = fc::build_start_pause(50, manufacturer_, serial_);
  const auto pause_id = pause["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(pause);
  ASSERT_TRUE(master_->wait_state([&](const json& s) {
    return action_status(s, pause_id) == "FINISHED" && s.value("paused", false) && !s.value("driving", true);
  }).has_value()) << master_->state()->dump();

  const auto goals_before = nav2_->goals().size();
  const auto second_id = fc::make_uuid();
  master_->send_instant(fc::build_cancel_order(51, manufacturer_, serial_, "NONE"));
  master_->send_order(order(second_id));
  ASSERT_TRUE(master_->wait_state([&](const json& s) { return s.value("orderId", "") == second_id; }).has_value());
  std::this_thread::sleep_for(500ms);
  const auto held = master_->state();
  EXPECT_TRUE(held->value("paused", false)) << held->dump();
  EXPECT_FALSE(held->value("driving", true));
  EXPECT_EQ(nav2_->goals().size(), goals_before);

  const auto resume = fc::build_stop_pause(52, manufacturer_, serial_);
  const auto resume_id = resume["actions"][0]["actionId"].get<std::string>();
  master_->send_instant(resume);
  ASSERT_TRUE(master_->wait_state([&](const json& s) {
    return action_status(s, resume_id) == "FINISHED" && !s.value("paused", true) && s.value("driving", false);
  }).has_value()) << master_->state()->dump();
  EXPECT_TRUE(nav2_heads_to(1.0));
}

TEST_P(SystemE2E, BridgeRestartAfterAnOrderUpdateFinishesTheRoute)
{
  const auto order_id = fc::make_uuid();
  master_->send_order(order(order_id, 0, 2));
  ASSERT_TRUE(nav2_heads_to(1.0));
  nav2_->succeed();
  ASSERT_TRUE(nav2_heads_to(2.0));
  master_->send_order(order(order_id, 1, 3, 2));
  ASSERT_TRUE(master_->wait_state([](const json& s) { return s.value("orderUpdateId", 0u) == 1u; }).has_value());

  const auto goals_before = nav2_->goals().size();
  restart_bridge();
  ASSERT_TRUE(wait_until([&] { return nav2_->goals().size() == goals_before + 1; }));
  EXPECT_EQ(nav2_->goals().back(), 2.0) << "resumed at the wrong node";
  ASSERT_TRUE(nav2_heads_to(2.0));
  nav2_->succeed();
  ASSERT_TRUE(nav2_heads_to(3.0));
  nav2_->succeed();

  const auto done = master_->wait_state([&](const json& s) {
    return fc::ParsedState(s).order_finished(order_id, "wp3");
  });
  ASSERT_TRUE(done.has_value()) << master_->state()->dump();
  EXPECT_TRUE(done->at("errors").empty()) << done->dump();
}

TEST_P(SystemE2E, BridgeGoneRaisesADriverErrorUntilItIsBack)
{
  if (std::string(rmw_get_implementation_identifier()) == "rmw_fastrtps_cpp") {
    GTEST_SKIP() << "Fast DDS reports no liveliness loss of a writer in the same process";
  }
  const auto has_driver_error = [](const json& s) {
    return std::any_of(s["errors"].begin(), s["errors"].end(),
                       [](const json& e) { return e.value("errorType", "") == "driverConnectionError"; });
  };
  const auto order_id = fc::make_uuid();
  master_->send_order(order(order_id));
  ASSERT_TRUE(nav2_heads_to(1.0));

  executor_->remove_node(bridge_);
  const auto options = bridge_->get_node_options();
  bridge_.reset();
  const auto lost = master_->wait_state([&](const json& s) { return has_driver_error(s) && !s.value("driving", true); });
  ASSERT_TRUE(lost.has_value()) << master_->state()->dump();
  EXPECT_EQ(lost->value("orderId", ""), order_id);

  const auto goals_before = nav2_->goals().size();
  bridge_ = std::make_shared<tb3_vda5050_bridge::BridgeNode>(options);
  executor_->add_node(bridge_);
  ASSERT_TRUE(master_->wait_state([&](const json& s) { return !has_driver_error(s); }).has_value());
  ASSERT_TRUE(wait_until([&] { return nav2_->goals().size() > goals_before; }));
  for (double x = 1.0; x <= 3.0; x += 1.0)
  {
    ASSERT_TRUE(nav2_heads_to(x)) << x;
    nav2_->succeed();
  }
  const auto done = master_->wait_state([&](const json& s) {
    return fc::ParsedState(s).order_finished(order_id, "wp3");
  });
  ASSERT_TRUE(done.has_value()) << master_->state()->dump();
  EXPECT_TRUE(done->at("errors").empty()) << done->dump();
}

INSTANTIATE_TEST_SUITE_P(Modes, SystemE2E, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Strict" : "Default"; });
