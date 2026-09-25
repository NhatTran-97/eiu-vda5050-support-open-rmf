/**
 * @file test_bridge_node.cpp
 * @brief BridgeNode against a fake Nav2 NavigateToPose server and a fake client adapter.
 *
 * Coverage:
 *  - NavigateToNode steps: Nav2 goal, edge-entered feedback, REACHED with distance, node already reached,
 *    position-less node, edge speed limit
 *  - preemption by a newer step, client cancel, local "cancel:" from robot tools, per-action commands
 *  - Nav2 goal cancelled by another client, rejected goals and the retry window, manual override
 *  - driver status: driving flag, a new session id per bridge process, liveliness lease and heartbeat
 *  - initPosition, unsupported actions, operating mode, battery, odometry telemetry
 */

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav2_msgs/msg/speed_limit.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <std_msgs/msg/string.hpp>
#include <vda5050_msgs/action/navigate_to_node.hpp>
#include <vda5050_msgs/msg/action.hpp>
#include <vda5050_msgs/msg/action_command.hpp>
#include <vda5050_msgs/msg/action_state.hpp>
#include <vda5050_msgs/msg/agv_position.hpp>
#include <vda5050_msgs/msg/battery_state.hpp>
#include <vda5050_msgs/msg/driver_status.hpp>
#include <vda5050_msgs/msg/velocity.hpp>

#include "tb3_vda5050_bridge/bridge_node.hpp"

using namespace std::chrono_literals;
using NavigateToPose = nav2_msgs::action::NavigateToPose;
using NavigateToNode = vda5050_msgs::action::NavigateToNode;
using Outcome = NavigateToNode::Result;

namespace {

class RosEnvironment : public ::testing::Environment {
public:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};
const auto* const kRosEnvironment = ::testing::AddGlobalTestEnvironment(new RosEnvironment);

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

rclcpp::NodeOptions isolated_options()
{
  rclcpp::NodeOptions options;
  options.use_global_arguments(false);
  return options;
}

// ─── Step builders ───────────────────────────────────────────────────────────

vda5050_msgs::msg::Node route_node(const std::string& id, uint32_t seq, double x)
{
  vda5050_msgs::msg::Node n;
  n.node_id = id;
  n.sequence_id = seq;
  n.released = true;
  n.node_position_set = true;
  n.node_position.x = x;
  n.node_position.y = 0.0;
  n.node_position.map_id = "map";
  n.node_position.allowed_deviation_xy = 0.2;
  n.node_position.allowed_deviation_theta = 3.14;
  return n;
}

// Step toward wp<i> at x = i, entered over the edge from wp<i-1> (none for i = 0).
NavigateToNode::Goal step_goal(const std::string& order_id, int i, double max_speed = -1.0)
{
  NavigateToNode::Goal goal;
  goal.order_id = order_id;
  goal.node = route_node("wp" + std::to_string(i), 2 * i, 1.0 * i);
  if (i > 0)
  {
    goal.incoming_edge.edge_id = "e_wp" + std::to_string(i - 1) + "_wp" + std::to_string(i);
    goal.incoming_edge.sequence_id = 2 * i - 1;
    goal.incoming_edge.released = true;
    goal.incoming_edge.max_speed = max_speed;
    goal.incoming_edge_set = true;
  }
  return goal;
}

// ─── Fakes ───────────────────────────────────────────────────────────────────

// NavigateToPose server that records goals and finishes them on request.
class FakeNav2 {
public:
  using GoalHandle = rclcpp_action::ServerGoalHandle<NavigateToPose>;

  FakeNav2(rclcpp::Node::SharedPtr node, const std::string& name) : node_(std::move(node))
  {
    server_ = rclcpp_action::create_server<NavigateToPose>(
      node_, name,
      [this](const rclcpp_action::GoalUUID&, std::shared_ptr<const NavigateToPose::Goal> goal) {
        std::lock_guard<std::mutex> l(mutex_);
        ++goals_received_;
        if (reject_) return rclcpp_action::GoalResponse::REJECT;
        goals_.push_back(goal->pose.pose.position.x);
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
      },
      [](const std::shared_ptr<GoalHandle>&) { return rclcpp_action::CancelResponse::ACCEPT; },
      [this](const std::shared_ptr<GoalHandle>& handle) {
        std::lock_guard<std::mutex> l(mutex_);
        for (auto& previous : active_)
        {
          if (previous->is_active()) previous->abort(std::make_shared<NavigateToPose::Result>());
        }
        active_.clear();
        active_.push_back(handle);
      });
    cancel_timer_ = node_->create_wall_timer(10ms, [this] {
      std::lock_guard<std::mutex> l(mutex_);
      for (auto& handle : active_)
      {
        if (handle->is_canceling())
        {
          handle->canceled(std::make_shared<NavigateToPose::Result>());
          ++cancelled_;
        }
      }
    });
  }

  void set_reject(bool reject) { std::lock_guard<std::mutex> l(mutex_); reject_ = reject; }
  std::vector<double> goals() const { std::lock_guard<std::mutex> l(mutex_); return goals_; }
  int goals_received() const { std::lock_guard<std::mutex> l(mutex_); return goals_received_; }
  int cancelled() const { std::lock_guard<std::mutex> l(mutex_); return cancelled_; }

  bool has_active_goal() const
  {
    std::lock_guard<std::mutex> l(mutex_);
    return !active_.empty() && active_.back()->is_active() && !active_.back()->is_canceling();
  }

  // Finish the active goal as succeeded.
  void succeed()
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!active_.empty() && active_.back()->is_active())
    {
      active_.back()->succeed(std::make_shared<NavigateToPose::Result>());
    }
  }

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp_action::Server<NavigateToPose>::SharedPtr server_;
  rclcpp::TimerBase::SharedPtr cancel_timer_;
  mutable std::mutex mutex_;
  bool reject_{false};
  int goals_received_{0};
  int cancelled_{0};
  std::vector<double> goals_;
  std::vector<std::shared_ptr<GoalHandle>> active_;
};

// Client adapter side of the bridge (NavigateToNode client, adapter topics), plus robot sensors.
class FakeRobot {
public:
  using StepHandle = rclcpp_action::ClientGoalHandle<NavigateToNode>;

  // What the client saw of one step goal.
  struct Step {
    std::string            node_id;
    bool                   accepted{false};
    int                    edge_entered{0};  // feedback messages with edge_entered set
    std::optional<uint8_t> outcome;   // result outcome, CANCELED when the code does not carry one
    double                 distance{0.0};
    StepHandle::SharedPtr  handle;
  };

  FakeRobot(rclcpp::Node::SharedPtr node, const std::string& ns) : node_(std::move(node)), ns_(ns)
  {
    const auto a = [&](const std::string& leaf) { return ns_ + "/adapter/" + leaf; };
    step_client_ = rclcpp_action::create_client<NavigateToNode>(node_, a("navigate_to_node"));
    local_pub_ = node_->create_publisher<std_msgs::msg::String>(a("action_cancel"), rclcpp::QoS(10));
    command_pub_ = node_->create_publisher<vda5050_msgs::msg::ActionCommand>(a("action_command"), rclcpp::QoS(10));
    execute_pub_ = node_->create_publisher<vda5050_msgs::msg::Action>(a("action_execute"), rclcpp::QoS(10));
    amcl_pub_ = node_->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
      ns_ + "/amcl_pose", rclcpp::QoS(10).transient_local());
    odom_pub_ = node_->create_publisher<nav_msgs::msg::Odometry>(ns_ + "/odom", rclcpp::QoS(10));
    battery_pub_ = node_->create_publisher<sensor_msgs::msg::BatteryState>(ns_ + "/battery", rclcpp::QoS(10));
    diagnostics_pub_ = node_->create_publisher<diagnostic_msgs::msg::DiagnosticArray>(ns_ + "/diagnostics", rclcpp::QoS(10));

    record<vda5050_msgs::msg::ActionState>(a("action_state_feedback"), feedback_, [](const vda5050_msgs::msg::ActionState& m) {
      return m.action_id + ":" + m.action_status; });
    record<vda5050_msgs::msg::Velocity>(a("velocity"), velocities_, [](const vda5050_msgs::msg::Velocity& m) { return std::to_string(m.vx); });
    record<geometry_msgs::msg::PoseWithCovarianceStamped>(ns_ + "/initialpose", initial_poses_, [](const geometry_msgs::msg::PoseWithCovarianceStamped& m) {
      return std::to_string(m.pose.pose.position.x); });
    record<nav2_msgs::msg::SpeedLimit>(ns_ + "/speed_limit", speed_limits_, [](const nav2_msgs::msg::SpeedLimit& m) {
      return std::to_string(m.speed_limit); });
    record<vda5050_msgs::msg::BatteryState>(a("battery_state"), batteries_, [](const vda5050_msgs::msg::BatteryState& m) {
      return std::to_string(static_cast<int>(std::round(m.battery_charge))); });
    record<vda5050_msgs::msg::AgvPosition>(a("agv_position"), positions_, [](const vda5050_msgs::msg::AgvPosition& m) {
      return m.position_initialized ? "valid" : "invalid"; });

    status_sub_ = node_->create_subscription<vda5050_msgs::msg::DriverStatus>(a("driver_status"), rclcpp::QoS(1).transient_local(),
      [this](vda5050_msgs::msg::DriverStatus::SharedPtr m) {
        std::lock_guard<std::mutex> l(mutex_);
        driving_ = m->driving;
        session_ = m->session_id;
        ++status_count_;
      });
    mode_sub_ = node_->create_subscription<std_msgs::msg::String>(a("operating_mode"), rclcpp::QoS(1).transient_local(),
      [this](std_msgs::msg::String::SharedPtr m) { std::lock_guard<std::mutex> l(mutex_); mode_ = m->data; });
  }

  // Send a step goal; returns its index in steps().
  std::size_t send(const NavigateToNode::Goal& goal)
  {
    std::size_t index;
    {
      std::lock_guard<std::mutex> l(mutex_);
      index = steps_.size();
      Step step;
      step.node_id = goal.node.node_id;
      steps_.push_back(step);
    }
    rclcpp_action::Client<NavigateToNode>::SendGoalOptions options;
    options.goal_response_callback = [this, index](const StepHandle::SharedPtr& handle) {
      std::lock_guard<std::mutex> l(mutex_);
      steps_[index].accepted = handle != nullptr;
      steps_[index].handle = handle;
    };
    options.feedback_callback = [this, index](StepHandle::SharedPtr, const std::shared_ptr<const NavigateToNode::Feedback> f) {
      std::lock_guard<std::mutex> l(mutex_);
      if (f->edge_entered) ++steps_[index].edge_entered;
    };
    options.result_callback = [this, index](const StepHandle::WrappedResult& r) {
      std::lock_guard<std::mutex> l(mutex_);
      // Same mapping as the client adapter: REACHED only with SUCCEEDED, other outcomes with ABORTED.
      uint8_t outcome = Outcome::CANCELED;
      if (r.code == rclcpp_action::ResultCode::SUCCEEDED) {
        outcome = Outcome::REACHED;
      } else if (r.code == rclcpp_action::ResultCode::ABORTED && r.result && r.result->outcome != Outcome::REACHED) {
        outcome = r.result->outcome;
      }
      steps_[index].outcome = outcome;
      steps_[index].distance = r.result ? r.result->distance_driven : 0.0;
    };
    step_client_->async_send_goal(goal, options);
    return index;
  }
  // Cancel step `index` once its goal is accepted.
  bool cancel(std::size_t index)
  {
    if (!wait_until([&] { return step(index).handle != nullptr; })) return false;
    step_client_->async_cancel_goal(step(index).handle);
    return true;
  }

  void local_command(const std::string& payload) { std_msgs::msg::String m; m.data = payload; local_pub_->publish(m); }
  void action_command(uint8_t command, const std::string& id)
  {
    vda5050_msgs::msg::ActionCommand m; m.command = command; m.action_id = id; command_pub_->publish(m);
  }
  void execute(const vda5050_msgs::msg::Action& a) { execute_pub_->publish(a); }

  void pose(double x, double covariance = 0.01)
  {
    geometry_msgs::msg::PoseWithCovarianceStamped m;
    m.header.frame_id = "map";
    m.pose.pose.position.x = x;
    m.pose.pose.orientation.w = 1.0;
    m.pose.covariance[0] = covariance;
    m.pose.covariance[7] = covariance;
    amcl_pub_->publish(m);
  }
  void odom(double x, double vx = 0.0)
  {
    nav_msgs::msg::Odometry m;
    m.pose.pose.position.x = x;
    m.twist.twist.linear.x = vx;
    odom_pub_->publish(m);
  }
  void battery(float percentage, float voltage)
  {
    sensor_msgs::msg::BatteryState m;
    m.percentage = percentage;
    m.voltage = voltage;
    battery_pub_->publish(m);
  }
  void twist_mux(bool joystick_unmasked)
  {
    diagnostic_msgs::msg::DiagnosticArray array;
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "twist_mux: Twist mux status";
    diagnostic_msgs::msg::KeyValue nav, joy;
    nav.key = "velocity topics.navigation";
    nav.value = "unmasked";
    joy.key = "velocity topics.joystick";
    joy.value = joystick_unmasked ? "unmasked" : "masked";
    status.values = {nav, joy};
    array.status.push_back(status);
    diagnostics_pub_->publish(array);
  }

  bool linked() const
  {
    return step_client_->action_server_is_ready() && local_pub_->get_subscription_count() > 0 &&
           command_pub_->get_subscription_count() > 0 && execute_pub_->get_subscription_count() > 0 &&
           odom_pub_->get_subscription_count() > 0 && battery_pub_->get_subscription_count() > 0;
  }
  bool diagnostics_linked() const { return diagnostics_pub_->get_subscription_count() > 0; }

  Step step(std::size_t index) const { std::lock_guard<std::mutex> l(mutex_); return steps_.at(index); }
  bool finished(std::size_t index, uint8_t outcome) const
  {
    std::lock_guard<std::mutex> l(mutex_);
    return steps_.at(index).outcome == outcome;
  }
  std::vector<std::string> feedback() const { return get(feedback_); }
  std::vector<std::string> velocities() const { return get(velocities_); }
  std::vector<std::string> initial_poses() const { return get(initial_poses_); }
  std::vector<std::string> speed_limits() const { return get(speed_limits_); }
  std::vector<std::string> batteries() const { return get(batteries_); }
  std::vector<std::string> positions() const { return get(positions_); }
  bool driving() const { std::lock_guard<std::mutex> l(mutex_); return driving_; }
  std::string session() const { std::lock_guard<std::mutex> l(mutex_); return session_; }
  int status_count() const { std::lock_guard<std::mutex> l(mutex_); return status_count_; }
  std::string mode() const { std::lock_guard<std::mutex> l(mutex_); return mode_; }

  bool has_feedback(const std::string& entry) const
  {
    const auto all = feedback();
    return std::find(all.begin(), all.end(), entry) != all.end();
  }

private:
  template <typename Msg, typename Format>
  void record(const std::string& topic, std::vector<std::string>& into, Format format)
  {
    subs_.push_back(node_->create_subscription<Msg>(topic, rclcpp::QoS(50),
      [this, &into, format](typename Msg::SharedPtr m) {
        std::lock_guard<std::mutex> l(mutex_);
        into.push_back(format(*m));
      }));
  }
  std::vector<std::string> get(const std::vector<std::string>& v) const
  {
    std::lock_guard<std::mutex> l(mutex_);
    return v;
  }

  rclcpp::Node::SharedPtr node_;
  std::string ns_;
  rclcpp_action::Client<NavigateToNode>::SharedPtr step_client_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr local_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::ActionCommand>::SharedPtr command_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::Action>::SharedPtr execute_pub_;
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr amcl_pub_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odom_pub_;
  rclcpp::Publisher<sensor_msgs::msg::BatteryState>::SharedPtr battery_pub_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_pub_;
  std::vector<rclcpp::SubscriptionBase::SharedPtr> subs_;
  rclcpp::Subscription<vda5050_msgs::msg::DriverStatus>::SharedPtr status_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mode_sub_;
  mutable std::mutex mutex_;
  std::vector<Step> steps_;
  std::vector<std::string> feedback_, velocities_, initial_poses_, speed_limits_, batteries_, positions_;
  bool driving_{false};
  std::string session_;
  int status_count_{0};
  std::string mode_;
};

}  // namespace

class BridgeNodeTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    static std::atomic<int> counter{0};
    ns_ = "/bridge_test_" + std::to_string(::getpid()) + "_" + std::to_string(++counter);

    fakes_node_ = std::make_shared<rclcpp::Node>("fakes", ns_, isolated_options());
    nav2_ = std::make_unique<FakeNav2>(fakes_node_, ns_ + "/navigate_to_pose");
    robot_ = std::make_unique<FakeRobot>(fakes_node_, ns_);
    executor_ = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    executor_->add_node(fakes_node_);
    spin_thread_ = std::thread([this] { while (!stop_.load()) executor_->spin_some(5ms); });

    robot_->pose(0.0);
    robot_->odom(0.0);
  }

  void TearDown() override
  {
    stop_bridge();
    stop_.store(true);
    if (spin_thread_.joinable()) spin_thread_.join();
    executor_.reset();
    robot_.reset();
    nav2_.reset();
    fakes_node_.reset();
  }

  // Start the bridge with test topics; extra overrides replace the defaults below.
  void start_bridge(std::vector<rclcpp::Parameter> extra = {})
  {
    auto options = isolated_options();
    std::vector<rclcpp::Parameter> params{
      {"adapter_ns", ns_ + "/adapter"},
      {"odom_topic", ns_ + "/odom"},
      {"amcl_pose_topic", ns_ + "/amcl_pose"},
      {"initial_pose_topic", ns_ + "/initialpose"},
      {"battery_topic", ns_ + "/battery"},
      {"speed_limit_topic", ns_ + "/speed_limit"},
      {"diagnostics_topic", ns_ + "/diagnostics"},
      {"nav2_action_name", ns_ + "/navigate_to_pose"},
      {"map_id", "map"},
      {"supported_action_types", std::vector<std::string>{"initPosition"}},
      {"nav2_dispatch_timeout_sec", 30.0},
      {"nav2_retry_period_sec", 0.2},
      {"odom_publish_min_interval_sec", 0.0},
    };
    for (const auto& p : extra)
    {
      auto it = std::find_if(params.begin(), params.end(), [&](const auto& q) { return q.get_name() == p.get_name(); });
      if (it != params.end()) *it = p; else params.push_back(p);
    }
    options.parameter_overrides(params);
    options.arguments({"--ros-args", "-r", "__ns:=" + ns_});
    bridge_ = std::make_shared<tb3_vda5050_bridge::BridgeNode>(options);
    executor_->add_node(bridge_);
    ASSERT_TRUE(wait_until([&] { return robot_->linked(); })) << "bridge topics not matched";
    ASSERT_TRUE(wait_until([&] { return !robot_->positions().empty(); })) << "no AMCL pose at the bridge";
  }

  void stop_bridge()
  {
    if (!bridge_) return;
    executor_->remove_node(bridge_);
    bridge_.reset();
  }

  // Send the step toward wp<i> of `order_id` and wait until Nav2 drives it.
  std::size_t start_step(const std::string& order_id, int i)
  {
    const auto index = robot_->send(step_goal(order_id, i));
    EXPECT_TRUE(wait_until([&] { return nav2_->has_active_goal() && nav2_->goals().back() == 1.0 * i; }))
      << "no Nav2 goal for wp" << i;
    EXPECT_TRUE(wait_until([&] { return robot_->driving(); }));
    return index;
  }

  std::string ns_;
  rclcpp::Node::SharedPtr fakes_node_;
  std::unique_ptr<FakeNav2> nav2_;
  std::unique_ptr<FakeRobot> robot_;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::shared_ptr<tb3_vda5050_bridge::BridgeNode> bridge_;
  std::thread spin_thread_;
  std::atomic<bool> stop_{false};
};

// ─── Steps ───────────────────────────────────────────────────────────────────

TEST_F(BridgeNodeTest, StepDrivesToTheNodeAndReportsTheDistance)
{
  start_bridge();
  const auto step = start_step("o1", 1);
  ASSERT_TRUE(wait_until([&] { return robot_->step(step).edge_entered == 1; }));

  for (int i = 0; i <= 10; ++i)
  {
    robot_->odom(0.1 * i, 0.2);
    std::this_thread::sleep_for(5ms);
  }
  std::this_thread::sleep_for(50ms);
  nav2_->succeed();
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::REACHED); }));
  EXPECT_NEAR(robot_->step(step).distance, 1.0, 1e-6);
  ASSERT_TRUE(wait_until([&] { return !robot_->driving(); }));
  EXPECT_EQ(nav2_->goals().size(), 1u);
}

TEST_F(BridgeNodeTest, NodeUnderTheRobotIsReachedWithoutNav2)
{
  start_bridge();
  const auto step = robot_->send(step_goal("o1", 0));
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::REACHED); }));
  EXPECT_EQ(robot_->step(step).edge_entered, 0);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(nav2_->goals_received(), 0);
}

TEST_F(BridgeNodeTest, PositionlessNodeIsReachedOnceThePoseIsValid)
{
  start_bridge();
  robot_->pose(0.0, 5.0);
  ASSERT_TRUE(wait_until([&] { return robot_->positions().back() == "invalid"; }));
  auto goal = step_goal("o1", 1);
  goal.node.node_position_set = false;
  const auto step = robot_->send(goal);
  std::this_thread::sleep_for(400ms);
  EXPECT_FALSE(robot_->step(step).outcome.has_value());

  robot_->pose(0.0);
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::REACHED); }));
  EXPECT_EQ(nav2_->goals_received(), 0);
}

TEST_F(BridgeNodeTest, EdgeMaxSpeedIsAppliedAsSpeedLimit)
{
  start_bridge();
  robot_->send(step_goal("o1", 1, 0.1));
  ASSERT_TRUE(wait_until([&] { return !robot_->speed_limits().empty(); }));
  EXPECT_EQ(robot_->speed_limits().back(), std::to_string(0.1));
}

TEST_F(BridgeNodeTest, NewerStepPreemptsTheActiveOne)
{
  start_bridge();
  const auto first = start_step("o1", 1);
  const auto second = start_step("o1", 2);
  ASSERT_TRUE(wait_until([&] { return robot_->finished(first, Outcome::PREEMPTED); }));
  EXPECT_EQ(nav2_->goals(), (std::vector<double>{1.0, 2.0}));

  nav2_->succeed();
  ASSERT_TRUE(wait_until([&] { return robot_->finished(second, Outcome::REACHED); }));
}

TEST_F(BridgeNodeTest, PreemptingStepAtTheRobotCancelsThePreviousNav2Goal)
{
  start_bridge();
  const auto first = start_step("o1", 1);
  const auto second = robot_->send(step_goal("o2", 0));
  ASSERT_TRUE(wait_until([&] { return robot_->finished(second, Outcome::REACHED); }));
  EXPECT_TRUE(robot_->finished(first, Outcome::PREEMPTED));
  ASSERT_TRUE(wait_until([&] { return nav2_->cancelled() == 1 && !nav2_->has_active_goal(); }));
  ASSERT_TRUE(wait_until([&] { return !robot_->driving(); }));
}

TEST_F(BridgeNodeTest, CancelledStepStopsNavigation)
{
  start_bridge();
  const auto step = start_step("o1", 1);
  ASSERT_TRUE(robot_->cancel(step));
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::CANCELED); }));
  ASSERT_TRUE(wait_until([&] { return nav2_->cancelled() == 1 && !robot_->driving(); }));

  const auto next = start_step("o1", 1);
  nav2_->succeed();
  ASSERT_TRUE(wait_until([&] { return robot_->finished(next, Outcome::REACHED); }));
}

TEST_F(BridgeNodeTest, LocalCancelDropsTheStep)
{
  start_bridge();
  const auto step = start_step("o1", 1);
  robot_->local_command("cancel:local-ui");
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::DROPPED); }));
  ASSERT_TRUE(wait_until([&] { return nav2_->cancelled() == 1 && !robot_->driving(); }));
}

TEST_F(BridgeNodeTest, OtherLocalCommandsAndActionCommandsDoNotTouchNavigation)
{
  start_bridge();
  const auto step = start_step("o1", 1);
  robot_->local_command("pause:p1");
  robot_->action_command(vda5050_msgs::msg::ActionCommand::PAUSE, "a1");
  robot_->action_command(vda5050_msgs::msg::ActionCommand::CANCEL, "a1");
  std::this_thread::sleep_for(400ms);
  EXPECT_TRUE(robot_->driving());
  EXPECT_FALSE(robot_->step(step).outcome.has_value());
  EXPECT_EQ(nav2_->cancelled(), 0);
}

// ─── Nav2 failures and retries ───────────────────────────────────────────────

TEST_F(BridgeNodeTest, GoalCancelledByAnotherClientIsSentAgain)
{
  start_bridge();
  const auto step = start_step("o1", 1);
  auto other = rclcpp_action::create_client<NavigateToPose>(fakes_node_, ns_ + "/navigate_to_pose");
  ASSERT_TRUE(other->wait_for_action_server(2s));
  other->async_cancel_all_goals();

  ASSERT_TRUE(wait_until([&] { return nav2_->cancelled() == 1; }));
  ASSERT_TRUE(wait_until([&] { return nav2_->goals().size() == 2u && nav2_->has_active_goal(); }, 3s));
  EXPECT_EQ(nav2_->goals().back(), 1.0);
  EXPECT_FALSE(robot_->step(step).outcome.has_value());
  ASSERT_TRUE(wait_until([&] { return robot_->driving(); }));
}

TEST_F(BridgeNodeTest, RejectedGoalsFailTheStepAfterTheRetryWindow)
{
  start_bridge({{"nav2_dispatch_timeout_sec", 1.0}});
  nav2_->set_reject(true);
  const auto step = robot_->send(step_goal("o1", 1));
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::FAILED); }));
  EXPECT_EQ(robot_->step(step).edge_entered, 0);
  EXPECT_GE(nav2_->goals_received(), 3);
}

TEST_F(BridgeNodeTest, ManualOverrideDoesNotSpendTheRetryWindow)
{
  start_bridge({{"nav2_dispatch_timeout_sec", 1.0}});
  ASSERT_TRUE(wait_until([&] { return robot_->diagnostics_linked(); }));
  robot_->twist_mux(true);
  ASSERT_TRUE(wait_until([&] { return robot_->mode() == "MANUAL"; }));
  nav2_->set_reject(true);
  const auto step = robot_->send(step_goal("o1", 1));
  std::this_thread::sleep_for(2000ms);
  EXPECT_FALSE(robot_->step(step).outcome.has_value());

  nav2_->set_reject(false);
  robot_->twist_mux(false);
  ASSERT_TRUE(wait_until([&] { return nav2_->has_active_goal() && robot_->driving(); }));
}

// ─── Driver status ───────────────────────────────────────────────────────────

TEST_F(BridgeNodeTest, EveryBridgeProcessHasItsOwnSession)
{
  start_bridge();
  ASSERT_TRUE(wait_until([&] { return !robot_->session().empty(); }));
  const auto first = robot_->session();
  EXPECT_EQ(first.size(), 16u);

  stop_bridge();
  start_bridge();
  ASSERT_TRUE(wait_until([&] { return robot_->session() != first; }));
  EXPECT_FALSE(robot_->driving());
}

TEST_F(BridgeNodeTest, DriverStatusOffersALivelinessLeaseAndAHeartbeat)
{
  start_bridge({{"driver_status_lease_sec", 0.3}});
  const auto infos = fakes_node_->get_publishers_info_by_topic(ns_ + "/adapter/driver_status");
  ASSERT_EQ(infos.size(), 1u);
  const auto qos = infos.front().qos_profile();
  EXPECT_EQ(qos.liveliness(), rclcpp::LivelinessPolicy::ManualByTopic);
  EXPECT_NEAR(qos.liveliness_lease_duration().seconds(), 0.3, 1e-6);
  EXPECT_EQ(qos.durability(), rclcpp::DurabilityPolicy::TransientLocal);

  ASSERT_TRUE(wait_until([&] { return robot_->status_count() > 0; }));
  const auto before = robot_->status_count();
  std::this_thread::sleep_for(1000ms);
  EXPECT_GE(robot_->status_count() - before, 7) << "heartbeat every lease / 3";
}

TEST_F(BridgeNodeTest, NonPositiveDriverStatusLeaseIsRejected)
{
  auto options = isolated_options();
  options.parameter_overrides({{"driver_status_lease_sec", 0.0}});
  EXPECT_THROW(tb3_vda5050_bridge::BridgeNode node(options), std::invalid_argument);
}

// ─── Actions ─────────────────────────────────────────────────────────────────

namespace {

vda5050_msgs::msg::Action init_position(const std::string& id, double x)
{
  vda5050_msgs::msg::Action action;
  action.action_id = id;
  action.action_type = "initPosition";
  for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{{"x", std::to_string(x)}, {"y", "0"}, {"theta", "0"}})
  {
    vda5050_msgs::msg::ActionParameter p;
    p.key = k;
    p.value = v;
    action.action_parameters.push_back(p);
  }
  return action;
}

}  // namespace

TEST_F(BridgeNodeTest, InitPositionPublishesTheInitialPoseAndDropsTheWaitingStep)
{
  start_bridge();
  robot_->pose(0.0, 5.0);
  ASSERT_TRUE(wait_until([&] { return robot_->positions().back() == "invalid"; }));
  auto goal = step_goal("o1", 1);
  goal.node.node_position_set = false;
  const auto step = robot_->send(goal);
  std::this_thread::sleep_for(200ms);

  robot_->execute(init_position("init-1", 1.5));
  ASSERT_TRUE(wait_until([&] { return robot_->has_feedback("init-1:FINISHED"); }));
  EXPECT_EQ(robot_->initial_poses(), std::vector<std::string>{std::to_string(1.5)});
  ASSERT_TRUE(wait_until([&] { return robot_->finished(step, Outcome::DROPPED); }));
}

TEST_F(BridgeNodeTest, InitPositionIsRefusedWhileNavigatingWithAValidPose)
{
  start_bridge();
  const auto step = start_step("o1", 1);
  robot_->execute(init_position("init-3", 1.5));
  ASSERT_TRUE(wait_until([&] { return robot_->has_feedback("init-3:FAILED"); }));
  EXPECT_TRUE(robot_->initial_poses().empty());
  EXPECT_FALSE(robot_->step(step).outcome.has_value());
}

TEST_F(BridgeNodeTest, InitPositionWithoutNumbersFails)
{
  start_bridge();
  vda5050_msgs::msg::Action action;
  action.action_id = "init-2";
  action.action_type = "initPosition";
  robot_->execute(action);
  ASSERT_TRUE(wait_until([&] { return robot_->has_feedback("init-2:FAILED"); }));
  EXPECT_TRUE(robot_->initial_poses().empty());
}

TEST_F(BridgeNodeTest, UnsupportedActionFails)
{
  start_bridge();
  vda5050_msgs::msg::Action action;
  action.action_id = "x-1";
  action.action_type = "liftFork";
  robot_->execute(action);
  ASSERT_TRUE(wait_until([&] { return robot_->has_feedback("x-1:FAILED"); }));
  EXPECT_TRUE(robot_->has_feedback("x-1:RUNNING"));
}

// ─── Telemetry ───────────────────────────────────────────────────────────────

TEST_F(BridgeNodeTest, ManualOverrideFromTwistMuxIsLatched)
{
  start_bridge();
  ASSERT_TRUE(wait_until([&] { return robot_->diagnostics_linked(); }));
  robot_->twist_mux(true);
  ASSERT_TRUE(wait_until([&] { return robot_->mode() == "MANUAL"; }));
  robot_->twist_mux(false);
  ASSERT_TRUE(wait_until([&] { return robot_->mode() == "AUTOMATIC"; }));
}

TEST_F(BridgeNodeTest, BatteryReadingsAreReportedInPercent)
{
  start_bridge();
  robot_->battery(87.0f, 12.0f);
  robot_->battery(0.5f, 12.0f);
  robot_->battery(std::nanf(""), 10.8f);
  ASSERT_TRUE(wait_until([&] { return robot_->batteries().size() == 3u; }));
  EXPECT_EQ(robot_->batteries(), (std::vector<std::string>{"87", "50", "50"}));
}

TEST_F(BridgeNodeTest, OdometryTelemetryIsThrottled)
{
  start_bridge({{"odom_publish_min_interval_sec", 0.1}});
  for (int i = 0; i < 100; ++i)
  {
    robot_->odom(0.01 * i, 0.2);
    std::this_thread::sleep_for(5ms);
  }
  std::this_thread::sleep_for(200ms);
  const auto count = robot_->velocities().size();
  EXPECT_GE(count, 3u);
  EXPECT_LE(count, 8u);
}
