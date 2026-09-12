#include "tb3_vda5050_bridge/bridge_node.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

namespace tb3_vda5050_bridge {

namespace {

// Threshold above which allowedDeviationTheta means heading doesn't matter at this node.
constexpr double kUnconstrainedThetaRad = 3.0;

// Wrap angle into (-pi, pi].
double normalize_angle(double angle)
{
  while (angle > M_PI) angle -= 2.0 * M_PI;
  while (angle < -M_PI) angle += 2.0 * M_PI;
  return angle;
}

// Get the path where we save order state, defaults to $HOME/.ros.
std::string default_order_state_path()
{
  const char* home = std::getenv("HOME");
  const std::string base = home ? std::string(home) + "/.ros" : "/tmp";
  return base + "/tb3_vda5050_bridge_order_state.txt";
}

// Look up a parameter value in an action, return empty string if not found.
std::string find_action_parameter(const vda5050_msgs::msg::Action& action, const std::string& key)
{
  for (const auto& parameter : action.action_parameters) {
    if (parameter.key == key) return parameter.value;
  }
  return "";
}

}  // namespace



BridgeNode::BridgeNode(const rclcpp::NodeOptions& options)
: rclcpp::Node("tb3_vda5050_bridge", options)
{
  map_id_ = declare_parameter<std::string>("map_id", "map");
  nav2_frame_id_ = declare_parameter<std::string>("nav2_frame_id", "map");
  position_covariance_threshold_ =  declare_parameter<double>("position_covariance_threshold", 0.5);
  adapter_ns_ = declare_parameter<std::string>("adapter_ns", "/vda5050_client_adapter");
  odom_topic_ = declare_parameter<std::string>("odom_topic", "/odom");
  amcl_pose_topic_ = declare_parameter<std::string>("amcl_pose_topic", "/amcl_pose");
  initial_pose_topic_ = declare_parameter<std::string>("initial_pose_topic", "/initialpose");
  speed_limit_topic_ = declare_parameter<std::string>("speed_limit_topic", "/speed_limit");
  battery_topic_ = declare_parameter<std::string>("battery_topic", "/battery_state");
  nav2_action_name_ = declare_parameter<std::string>("nav2_action_name", "navigate_to_pose");

  supported_action_types_ = declare_parameter<std::vector<std::string>>("supported_action_types", std::vector<std::string>{});
  order_state_path_ = declare_parameter<std::string>("order_state_path", default_order_state_path());
  nav2_dispatch_timeout_sec_ =declare_parameter<double>("nav2_dispatch_timeout_sec", 120.0);
  amcl_pose_timeout_sec_ =declare_parameter<double>("amcl_pose_timeout_sec", 10.0);
  pose_stale_move_tolerance_m_ =declare_parameter<double>("pose_stale_move_tolerance_m", 0.15);
  using std::placeholders::_1;

  odom_sub_ = create_subscription<nav_msgs::msg::Odometry>(odom_topic_, rclcpp::QoS(10),std::bind(&BridgeNode::on_odom, this, _1));

  amcl_pose_sub_ = create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(amcl_pose_topic_, rclcpp::QoS(10).transient_local(), 
                                                                                      std::bind(&BridgeNode::on_amcl_pose, this, _1));

  battery_sub_ = create_subscription<sensor_msgs::msg::BatteryState>(battery_topic_, rclcpp::QoS(10),std::bind(&BridgeNode::on_battery, this, _1));

  order_sub_ = create_subscription<vda5050_msgs::msg::Order>(adapter_topic("order"), rclcpp::QoS(10).transient_local(),std::bind(&BridgeNode::on_order, this, _1));

  action_cancel_sub_ = create_subscription<std_msgs::msg::String>(adapter_topic("action_cancel"), rclcpp::QoS(10),std::bind(&BridgeNode::on_action_cancel, this, _1));

  action_execute_sub_ = create_subscription<vda5050_msgs::msg::Action>(adapter_topic("action_execute"), rclcpp::QoS(10),
                                                                      std::bind(&BridgeNode::on_action_execute, this, _1));

  initial_pose_pub_ = create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(initial_pose_topic_, rclcpp::QoS(10));
  speed_limit_pub_ = create_publisher<nav2_msgs::msg::SpeedLimit>(speed_limit_topic_, rclcpp::QoS(10));

  agv_position_pub_ = create_publisher<vda5050_msgs::msg::AgvPosition>(adapter_topic("agv_position"), rclcpp::QoS(10));
  battery_state_pub_ = create_publisher<vda5050_msgs::msg::BatteryState>(adapter_topic("battery_state"), rclcpp::QoS(10));
  velocity_pub_ = create_publisher<vda5050_msgs::msg::Velocity>(adapter_topic("velocity"), rclcpp::QoS(10));

  driving_pub_ = create_publisher<std_msgs::msg::Bool>(adapter_topic("driving"), rclcpp::QoS(1).transient_local());
  paused_pub_ = create_publisher<std_msgs::msg::Bool>(adapter_topic("paused"), rclcpp::QoS(1).transient_local());
  node_reached_pub_ = create_publisher<vda5050_msgs::msg::NodeState>(adapter_topic("node_reached"), rclcpp::QoS(10));
  edge_entered_pub_ = create_publisher<vda5050_msgs::msg::EdgeState>(adapter_topic("edge_entered"), rclcpp::QoS(10));
  edge_completed_pub_ = create_publisher<vda5050_msgs::msg::EdgeState>(adapter_topic("edge_completed"), rclcpp::QoS(10));
  action_state_feedback_pub_ = create_publisher<vda5050_msgs::msg::ActionState>(adapter_topic("action_state_feedback"), rclcpp::QoS(10));
  error_pub_ = create_publisher<vda5050_msgs::msg::Error>(adapter_topic("error"), rclcpp::QoS(10));
  order_dropped_pub_ = create_publisher<std_msgs::msg::String>(adapter_topic("order_dropped"), rclcpp::QoS(10));

  nav2_client_ = rclcpp_action::create_client<NavigateToPose>(this, nav2_action_name_);

  publish_bridge_status();
  RCLCPP_INFO(
    get_logger(),
    "TB3 VDA5050 Bridge started - map_id=%s nav2_frame_id=%s adapter_ns=%s nav2_action=%s",
    map_id_.c_str(),
    nav2_frame_id_.c_str(),
    adapter_ns_.c_str(),
    nav2_action_name_.c_str());
}

// Receive odometry (msg) and send velocity updates to adapter, accumulate real distance driven.
void BridgeNode::on_odom(const nav_msgs::msg::Odometry::SharedPtr msg)
{
  vda5050_msgs::msg::Velocity vel;
  vel.vx = msg->twist.twist.linear.x;
  vel.vy = 0.0;
  vel.omega = msg->twist.twist.angular.z;
  velocity_pub_->publish(vel);

  const double odom_x = msg->pose.pose.position.x;
  const double odom_y = msg->pose.pose.position.y;
  if (last_odom_position_valid_) 
  {
    distance_since_last_node_ += std::hypot(odom_x - last_odom_x_, odom_y - last_odom_y_);
  }
  last_odom_x_ = odom_x;
  last_odom_y_ = odom_y;
  last_odom_position_valid_ = true;
}

// Receive AMCL pose (msg), update cached position and confidence, republish to adapter.
void BridgeNode::on_amcl_pose(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
{
  tf2::Quaternion q;
  tf2::fromMsg(msg->pose.pose.orientation, q);
  tf2::Matrix3x3 m(q);
  double roll = 0.0, pitch = 0.0, yaw = 0.0;
  m.getRPY(roll, pitch, yaw);

  const double x = msg->pose.pose.position.x;
  const double y = msg->pose.pose.position.y;
  const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(yaw);

  // Row-major 6x6 covariance: index 0 = var(x), index 7 = var(y); both must pass the threshold.
  const double cov_x = msg->pose.covariance[0];
  const double cov_y = msg->pose.covariance[7];
  const bool covariance_ok = (cov_x >= 0.0 && cov_x < position_covariance_threshold_) && (cov_y >= 0.0 && cov_y < position_covariance_threshold_);

  last_amcl_pose_at_ = std::chrono::steady_clock::now();
  robot_pose_confident_ = finite && covariance_ok;

  // A non-finite reading leaves robot_pose_confident_ false without touching the last known-good pose.
  if (finite) {
    robot_x_   = x;
    robot_y_   = y;
    robot_yaw_ = yaw;
  }

  // Baseline for robot_pose_valid()'s stale-but-stationary check.
  if (robot_pose_confident_ && last_odom_position_valid_) 
  {
    odom_x_at_last_amcl_pose_ = last_odom_x_;
    odom_y_at_last_amcl_pose_ = last_odom_y_;
    odom_at_last_amcl_pose_valid_ = true;
    has_driven_since_last_amcl_pose_ = false;
  }

  vda5050_msgs::msg::AgvPosition pos;
  pos.x = robot_x_;
  pos.y = robot_y_;
  pos.theta = robot_yaw_;
  pos.map_id = map_id_;
  pos.position_initialized = robot_pose_confident_;
  pos.localization_score = -1.0;
  pos.deviation_range = -1.0;
  agv_position_pub_->publish(pos);
}

bool BridgeNode::robot_pose_valid() const
{
  if (!robot_pose_confident_) {
    return false;
  }
  const auto elapsed = std::chrono::steady_clock::now() - last_amcl_pose_at_;
  if (elapsed < std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(amcl_pose_timeout_sec_))) {
    return true;
  }
  // Past the timeout, trust the pose if the robot hasn't driven since the last confirmation.
  if (!has_driven_since_last_amcl_pose_) {
    return true;
  }
  if (!odom_at_last_amcl_pose_valid_ || !last_odom_position_valid_) {
    return false;
  }
  const double moved_since = std::hypot(
    last_odom_x_ - odom_x_at_last_amcl_pose_,
    last_odom_y_ - odom_y_at_last_amcl_pose_);
  return moved_since < pose_stale_move_tolerance_m_;
}

// Read battery (msg) in either TB3 or ROS format, normalize to 0-100%, publish.
void BridgeNode::on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg)
{
  vda5050_msgs::msg::BatteryState batt;

  // TB3 OpenCR publishes percentage as 0–100; standard ROS sensor_msgs uses 0.0–1.0 — handle both.
  const float pct = msg->percentage;
  float battery_charge;
  bool have_reading = true;
  if (pct > 1.0f) 
  {
    // TB3 style: already 0–100
    battery_charge = std::clamp(pct, 0.0f, 100.0f);

  } else if (!std::isnan(pct) && pct >= 0.0f) 
  {
    // Standard ROS 0.0–1.0
    battery_charge = pct * 100.0f;
  } 
  else if (msg->voltage > 1.0f) 
  {
    constexpr float V_FULL = 12.6f, V_EMPTY = 9.0f;
    battery_charge = std::clamp((msg->voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f, 0.0f, 100.0f);
  } 
  else 
  {
    have_reading = false;
    battery_charge = last_battery_charge_;
    if (!last_battery_valid_) 
    {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 30000,
        "Battery percentage and voltage both invalid, no prior reading — ""not publishing battery_state yet");
      return;
    }
  }
  if (have_reading) 
  {
    last_battery_charge_ = battery_charge;
    last_battery_valid_ = true;
  }
  batt.battery_charge = battery_charge;   // VDA5050: 0–100 %
  batt.battery_voltage = msg->voltage;
  batt.battery_health = -1;
  batt.charging = (msg->power_supply_status == sensor_msgs::msg::BatteryState::POWER_SUPPLY_STATUS_CHARGING);
  batt.reach = 0;
  battery_state_pub_->publish(batt);
}

// Starts a new order, or merges an update into the active one, then (re)dispatches.
void BridgeNode::on_order(const vda5050_msgs::msg::Order::SharedPtr msg)
{
  const bool is_update =
    order_session_.has_order() && order_session_.order_id() == msg->order_id;

  if (is_update) {

    if (!order_session_.update(*msg)) {
      RCLCPP_WARN(get_logger(), "Order update for %s rejected: updateId=%u is not newer than the ""current one — ignoring stale/duplicate update",
        msg->order_id.c_str(), msg->order_update_id);
      return;
    }

    RCLCPP_INFO(get_logger(), "Order update: id=%s updateId=%u nodes=%zu", msg->order_id.c_str(), msg->order_update_id, msg->nodes.size());

    const auto mode = state_machine_.mode();
    if (mode == BridgeMode::WAITING_FOR_RELEASE || mode == BridgeMode::IDLE) \
    {
      dispatch_next_work();
    }
    return;
  }

  RCLCPP_INFO(get_logger(), "Order received: id=%s nodes=%zu", msg->order_id.c_str(), msg->nodes.size());

  std::string persisted_order_id;
  std::size_t persisted_cursor = 0;
  bool persisted_terminal = false;
  const bool resuming_known_order = load_order_state(persisted_order_id, persisted_cursor, persisted_terminal) && persisted_order_id == msg->order_id;

  if (resuming_known_order && persisted_terminal) 
  {
    RCLCPP_WARN( get_logger(), "Ignoring order %s: already finished per persisted state", msg->order_id.c_str());
    return;
  }

  const std::size_t resume_cursor = resuming_known_order ? persisted_cursor : 0;

  auto previous_goal = current_goal_handle_;
  invalidate_navigation_context();
  goal_pending_preemption_ = previous_goal;
  order_session_.start(*msg, resume_cursor);

  distance_since_last_node_ = 0.0;
  state_machine_.on_order_started();
  publish_bridge_status();
  goal_sent_this_dispatch_ = false;
  dispatch_next_work();

  if (previous_goal && !goal_sent_this_dispatch_) 
  {
    nav2_client_->async_cancel_goal(previous_goal);
    goal_pending_preemption_.reset();
    RCLCPP_INFO(get_logger(), "Cancelled previous navigation (no replacement goal)");
  }
}

// Parse action_cancel (msg) payload ("pause:", "resume:", "cancel:") and dispatch accordingly.
void BridgeNode::on_action_cancel(const std_msgs::msg::String::SharedPtr msg)
{
  const std::string& data = msg->data;

  if (data.rfind("pause:", 0) == 0) {
    RCLCPP_INFO(get_logger(), "Pause signal received");
    cancel_navigation();
    state_machine_.on_pause_requested();
    publish_bridge_status();
    return;
  }

  if (data.rfind("resume:", 0) == 0) {
    RCLCPP_INFO(get_logger(), "Resume signal received");
    state_machine_.on_resume_requested();
    publish_bridge_status();
    dispatch_next_work();
    return;
  }

  if (data.rfind("cancel:", 0) == 0) {
    RCLCPP_INFO(get_logger(), "Cancel signal received");
    const auto cancelled_order_id = order_session_.order_id();
    const auto cancelled_cursor = order_session_.current_node_index();
    cancel_nav2_retry();
    cancel_navigation();
    order_session_.clear();
    state_machine_.on_cancel_requested();
    publish_bridge_status();
    if (!cancelled_order_id.empty())
    {
      persist_order_state(cancelled_order_id, cancelled_cursor, true);
      // Covers robot_local_ui, which publishes "cancel:" straight to this topic and never
      // goes through vda5050_client_adapter's own OrderManager -- a no-op if the adapter
      // already cleared it itself (its own cancelOrder flow does that before publishing here).
      notify_order_dropped(cancelled_order_id);
    }
    return;
  }

  RCLCPP_WARN(get_logger(), "Unknown action_cancel payload: %s", data.c_str());
}

// Process action (msg): handle initPosition, no-op others; returns success/failure feedback.
void BridgeNode::on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg)
{
  RCLCPP_INFO(get_logger(),"Action execute: id=%s type=%s",msg->action_id.c_str(), msg->action_type.c_str());

  publish_action_feedback(*msg, "RUNNING");

  const bool supported = std::find(supported_action_types_.begin(), supported_action_types_.end(),msg->action_type) != supported_action_types_.end();

  if (!supported) 
  {

    RCLCPP_WARN(get_logger(), "Unsupported action type '%s' (id=%s) — reporting FAILED",msg->action_type.c_str(), msg->action_id.c_str());
    publish_action_feedback(*msg, "FAILED", "Action type '" + msg->action_type + "' not supported by tb3_vda5050_bridge");
    return;
  }

  if (msg->action_type == "initPosition") {
    init_position(*msg);
    return;
  }

  publish_action_feedback(*msg, "FINISHED", "Completed (no-op handler)");
}

// Send operator's x/y/theta (from action) to AMCL for initial pose; fails while Nav2 is driving.
void BridgeNode::init_position(const vda5050_msgs::msg::Action& action)
{
  // Only a live Nav2 goal blocks re-localizing -- that is the case the guard exists for, since
  // moving the believed position mid-goal sends the robot down a path computed for elsewhere.
  // Order-session state must NOT gate this: an order whose navigation was interrupted (process
  // restart, Nav2 unavailable, a goal that never resolved) stays "in progress" forever with
  // nothing driving it, and re-localizing is exactly how an operator recovers from that.
  if (current_goal_handle_) {
      RCLCPP_WARN(get_logger(),"initPosition (id=%s) rejected: robot is executing a navigation goal",
                                    action.action_id.c_str());
                                    publish_action_feedback( action, "FAILED","initPosition refused while the robot is navigating — cancel or finish ""the current order first");
      return;
  }

  double x = 0.0, y = 0.0, theta = 0.0;
  try {
    x = std::stod(find_action_parameter(action, "x"));
    y = std::stod(find_action_parameter(action, "y"));
    theta = std::stod(find_action_parameter(action, "theta"));
  } catch (const std::exception&) {
    RCLCPP_WARN(get_logger(),"initPosition (id=%s) missing/invalid numeric x/y/theta parameters",action.action_id.c_str());
    publish_action_feedback(action, "FAILED", "initPosition requires numeric x, y, theta parameters");
    return;
  }

  geometry_msgs::msg::PoseWithCovarianceStamped pose;
  pose.header.frame_id = nav2_frame_id_;
  pose.header.stamp = now();
  pose.pose.pose.position.x = x;
  pose.pose.pose.position.y = y;
  tf2::Quaternion q;
  q.setRPY(0.0, 0.0, theta);
  pose.pose.pose.orientation = tf2::toMsg(q);
  // Same covariance rviz2's "2D Pose Estimate" tool and Nav2's own defaults use for a manually-supplied initial pose.
  pose.pose.covariance[6 * 0 + 0] = 0.25;
  pose.pose.covariance[6 * 1 + 1] = 0.25;
  pose.pose.covariance[6 * 5 + 5] = 0.06853891945200942;

  initial_pose_pub_->publish(pose);
  RCLCPP_INFO(get_logger(), "initPosition (id=%s): published initial pose (%.2f, %.2f, %.2f rad) to %s", action.action_id.c_str(), x, y, theta, initial_pose_topic_.c_str());

  // Any order still tracked here was planned from the pose just invalidated, so it can never be
  // resumed correctly -- drop it rather than leave it blocking the next dispatch.
  if (order_session_.has_order()) {
    const auto dropped_order_id = order_session_.order_id();
    const auto dropped_cursor = order_session_.current_node_index();
    cancel_nav2_retry();
    order_session_.clear();
    state_machine_.on_cancel_requested();
    publish_bridge_status();
    persist_order_state(dropped_order_id, dropped_cursor, true);
    notify_order_dropped(dropped_order_id);
    RCLCPP_INFO(get_logger(), "Dropped order %s: its start pose was invalidated by initPosition", dropped_order_id.c_str());
  }

  publish_action_feedback(action, "FINISHED", "Initial pose published to AMCL");
}

void BridgeNode::apply_speed_limit(double max_speed)
{
  nav2_msgs::msg::SpeedLimit msg;
  msg.header.stamp = now();
  msg.percentage = false;
  msg.speed_limit = max_speed >= 0.0 ? max_speed : 0.0;
  speed_limit_pub_->publish(msg);
  if (max_speed >= 0.0) {
    RCLCPP_INFO(get_logger(), "Applying edge speed limit: %.3f m/s", max_speed);
  }
}

// Internal: drive order forward, no return value; keeps retrying until success/timeout.
void BridgeNode::dispatch_next_work()
{
  if (state_machine_.is_paused()) {
    return;
  }


  while (true) {
    // A position-less node has no coordinate to verify — checks localisation only.
    if (order_session_.next_node_requires_pose_to_complete() && !robot_pose_valid()) {
      RCLCPP_WARN(
        get_logger(),
        "Robot pose not valid/confident — holding position-less node, will retry");
      state_machine_.on_dispatching();
      publish_bridge_status();
      arm_nav2_retry();
      return;
    }

    const auto plan = order_session_.plan_next_work();
    publish_traversal_events(plan.immediate_events);

    switch (plan.kind) {
      case DispatchKind::NAVIGATE:
        if (try_complete_in_place(*plan.target)) {
          continue;  // node reached without Nav2; advance to the next one
        }
        state_machine_.on_dispatching();
        publish_bridge_status();
        persist_order_state(
          order_session_.order_id(), order_session_.current_node_index(), false);
        send_navigation_goal(*plan.target);
        return;

      case DispatchKind::WAITING_FOR_RELEASE:
        state_machine_.on_waiting_for_release();
        publish_bridge_status();
        persist_order_state(
          order_session_.order_id(), order_session_.current_node_index(), false);
        RCLCPP_INFO(
          get_logger(),
          "Order %s is waiting for more released nodes at index %zu",
          order_session_.order_id().c_str(),
          order_session_.current_node_index());
        return;

      case DispatchKind::COMPLETED:
      default:
        cancel_nav2_retry();
        state_machine_.on_all_work_completed();
        publish_bridge_status();
        if (order_session_.has_order()) {
          persist_order_state(
            order_session_.order_id(), order_session_.current_node_index(), true);
          RCLCPP_INFO(
            get_logger(),
            "All work completed for order %s",
            order_session_.order_id().c_str());
        }
        return;
    }
  }
}


// Check if robot is near target (target): returns true if close enough; completes node locally.
bool BridgeNode::try_complete_in_place(const NavigationTarget& target)
{
  if (!robot_pose_valid()) {
    return false;
  }

  // Only short-circuits on the map the robot's pose is actually expressed in.
  if (!target.node.node_position.map_id.empty() &&
      target.node.node_position.map_id != map_id_) {
    return false;
  }

  const double dx = target.node.node_position.x - robot_x_;
  const double dy = target.node.node_position.y - robot_y_;
  const double dist_sq = dx * dx + dy * dy;
  const double xy_tol = target.node.node_position.allowed_deviation_xy > 0.0 ? target.node.node_position.allowed_deviation_xy : 0.5;
  if (dist_sq >= xy_tol * xy_tol) {
    return false;
  }

  // Falls through to Nav2 for an in-place rotation if heading matters and yaw is out of tolerance.
  const bool theta_constrained = target.node.node_position.theta_set && target.node.node_position.allowed_deviation_theta < kUnconstrainedThetaRad;
  if (theta_constrained) 
  {
    const double yaw_error = std::fabs(normalize_angle(target.node.node_position.theta - robot_yaw_));
    if (yaw_error >= target.node.node_position.allowed_deviation_theta) 
    {
      return false;
    }
  }

  RCLCPP_INFO(get_logger(),"Skipping Nav2 for node %s — robot already within %.2fm of target",target.node.node_id.c_str(), std::sqrt(dist_sq));
  if (target.incoming_edge.has_value()) {
    edge_entered_pub_->publish(*target.incoming_edge);
  }
  const auto events = order_session_.complete_navigation(target.node_index);
  if (events.empty()) 
  {
    RCLCPP_WARN(get_logger(),"complete_navigation(%zu) returned no events for node %s — index mismatch?",target.node_index, target.node.node_id.c_str());
  }
  publish_traversal_events(events);
  return true;
}

// Sends the Nav2 goal for `target`, or arms a retry if Nav2 isn't ready yet.
void BridgeNode::send_navigation_goal(const NavigationTarget& target)
{
  if (!nav2_client_->action_server_is_ready()) {
    RCLCPP_WARN(get_logger(),"Nav2 action server not available yet — holding node %s, will retry",target.node.node_id.c_str());
    state_machine_.on_dispatching();
    publish_bridge_status();
    arm_nav2_retry();
    return;
  }
  cancel_nav2_retry();
  apply_speed_limit(target.incoming_edge_max_speed);

  NavigateToPose::Goal goal;

  goal.pose.header.frame_id = nav2_frame_id_;
  goal.pose.header.stamp = now();
  goal.pose.pose.position.x = target.node.node_position.x;
  goal.pose.pose.position.y = target.node.node_position.y;

  // When heading isn't constrained, aims the goal yaw along the bearing to this node so
  // the robot arrives already facing it, skipping an unnecessary Nav2 rotate-in-place.
  const bool theta_constrained = target.node.node_position.theta_set && target.node.node_position.allowed_deviation_theta < kUnconstrainedThetaRad;

  double goal_yaw = target.node.node_position.theta;
  if (!theta_constrained && robot_pose_valid()) 
  {
    const double dx = target.node.node_position.x - robot_x_;
    const double dy = target.node.node_position.y - robot_y_;
    goal_yaw = (std::hypot(dx, dy) > 1e-3) ? std::atan2(dy, dx) : robot_yaw_;
  }

  tf2::Quaternion q;
  q.setRPY(0.0, 0.0, goal_yaw);
  goal.pose.pose.orientation = tf2::toMsg(q);

  const uint64_t token = ++navigation_token_;
  const uint64_t generation = order_session_.generation();
  const std::size_t node_index = target.node_index;
  const std::string node_id = target.node.node_id;

  RCLCPP_INFO(get_logger(),"Dispatching node %s (seq=%u) with token=%llu", target.node.node_id.c_str(), target.node.sequence_id, static_cast<unsigned long long>(token));

  auto send_goal_options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();

  send_goal_options.goal_response_callback =
    [this, token, generation, node_id, incoming_edge = target.incoming_edge](
      const GoalHandle::SharedPtr& handle) {
      if (token != navigation_token_ || generation != order_session_.generation()) {
        return;
      }

      auto preempted_goal = goal_pending_preemption_;
      goal_pending_preemption_.reset();

      if (!handle) {
        state_machine_.on_navigation_failed();
        publish_bridge_status();
        publish_navigation_error("Nav2 goal rejected", node_id);
        if (preempted_goal) {
                                    nav2_client_->async_cancel_goal(preempted_goal);
                                    RCLCPP_WARN(get_logger(),
                                    "Replacement goal for node %s was rejected — cancelling the goal it " "was meant to preempt instead of leaving it running unmanaged",
                                    node_id.c_str());
        }
        return;
      }

      // Published on accept, not send: a rejected goal must not leave this edge without a completion.
      if (incoming_edge.has_value()) {
        edge_entered_pub_->publish(*incoming_edge);
      }

      current_goal_handle_ = handle;
      state_machine_.on_navigation_active();
      publish_bridge_status();
      RCLCPP_INFO(get_logger(), "Nav2 goal accepted for node %s (token=%llu)", node_id.c_str(), static_cast<unsigned long long>(token));
    };

  send_goal_options.result_callback =
    [this, token, generation, node_index, node_id](const GoalHandle::WrappedResult& result) {
      if (token != navigation_token_ || generation != order_session_.generation()) {
        RCLCPP_DEBUG(get_logger(),"Ignoring stale Nav2 result for node %s (token=%llu)",node_id.c_str(),static_cast<unsigned long long>(token));
        return;
      }

      current_goal_handle_.reset();

      if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
        const auto events = order_session_.complete_navigation(node_index);
        if (events.empty()) 
        {
          RCLCPP_WARN(get_logger(),"complete_navigation(%zu) returned no events for node %s — index mismatch?", node_index, node_id.c_str());
        }
        publish_traversal_events(events);
        dispatch_next_work();
        return;
      }

      if (result.code == rclcpp_action::ResultCode::CANCELED) {
        RCLCPP_INFO(get_logger(), "Navigation to %s cancelled", node_id.c_str());
        return;
      }

      // Retries the same node via arm_nav2_retry() until nav2_dispatch_timeout_sec_ is exhausted.
      RCLCPP_WARN(get_logger(),"Navigation to %s failed (code=%d) — will retry", node_id.c_str(), static_cast<int>(result.code));
      state_machine_.on_dispatching();
      publish_bridge_status();
      arm_nav2_retry();
    };

  goal_sent_this_dispatch_ = true;
  nav2_client_->async_send_goal(goal, send_goal_options);
}

// Publish traversal events (events) with distance stamped, reset distance counter.
void BridgeNode::publish_traversal_events(const std::vector<TraversalEvent>& events)
{
  for (const auto& event : events) {
    if (event.edge_entered.has_value()) {
      edge_entered_pub_->publish(*event.edge_entered);
    }
    if (event.edge_completed.has_value()) {
      edge_completed_pub_->publish(*event.edge_completed);
    }
    if (event.node_reached.has_value()) {
      auto node_state = *event.node_reached;
      node_state.distance_driven = distance_since_last_node_;
      distance_since_last_node_ = 0.0;
      node_reached_pub_->publish(node_state);
    }
  }
}

// Publishes a VDA5050 ActionState update for `action`.
void BridgeNode::publish_action_feedback(const vda5050_msgs::msg::Action& action,
                                         const std::string& status, const std::string& description)
{
  vda5050_msgs::msg::ActionState state;
  state.action_id = action.action_id;
  state.action_type = action.action_type;
  state.action_description = action.action_description;
  state.action_status = status;
  state.result_description = description;
  action_state_feedback_pub_->publish(state);
}

// Publishes a VDA5050 navigationError with the optional `node_id`.
void BridgeNode::publish_navigation_error(const std::string& description, const std::string& node_id)
{
  vda5050_msgs::msg::Error error;
  error.error_type = "navigationError";

  error.error_level = "FATAL";
  error.error_description = description;

  vda5050_msgs::msg::ErrorReference order_ref;
  order_ref.reference_key = "orderId";
  order_ref.reference_value = order_session_.order_id();
  error.error_references.push_back(order_ref);

  if (!node_id.empty()) {
    vda5050_msgs::msg::ErrorReference node_ref;
    node_ref.reference_key = "nodeId";
    node_ref.reference_value = node_id;
    error.error_references.push_back(node_ref);
  }

  error_pub_->publish(error);
}

// Publish driving/paused state, re-anchor odometry baseline on driving transitions.
void BridgeNode::publish_bridge_status()
{
  const auto status = state_machine_.status();
  const bool was_driving = last_driving_.has_value() && *last_driving_;
  if (status.driving) {
    // Real drive happened; robot_pose_valid()'s stale check now cares about it.
    has_driven_since_last_amcl_pose_ = true;
  } else if (was_driving && last_odom_position_valid_) {
    // Just came to a stop — re-anchors the baseline instead of waiting for the next AMCL confirmation.
    odom_x_at_last_amcl_pose_ = last_odom_x_;
    odom_y_at_last_amcl_pose_ = last_odom_y_;
    odom_at_last_amcl_pose_valid_ = true;
    has_driven_since_last_amcl_pose_ = false;
  }
  set_driving(status.driving);
  set_paused(status.paused);
}

std::string BridgeNode::adapter_topic(const std::string& leaf) const
{
  if (adapter_ns_.empty()) {
    return "/" + leaf;
  }
  if (adapter_ns_.back() == '/') {
    return adapter_ns_ + leaf;
  }
  return adapter_ns_ + "/" + leaf;
}

// Cancel current Nav2 goal if active, no return value.
void BridgeNode::cancel_navigation()
{
  if (!current_goal_handle_) {
    invalidate_navigation_context();
    return;
  }

  auto goal_handle = current_goal_handle_;
  invalidate_navigation_context();
  nav2_client_->async_cancel_goal(goal_handle);
  RCLCPP_INFO(get_logger(), "Navigation cancel requested");
}

// Increment navigation_token_ to invalidate pending Nav2 results.
void BridgeNode::invalidate_navigation_context()
{
  ++navigation_token_;
  current_goal_handle_.reset();
}

// Start retry timer: calls dispatch_next_work() every 2s until timeout, no return value.
void BridgeNode::arm_nav2_retry()
{
  const auto generation = order_session_.generation();
  const auto node_index = order_session_.current_node_index();
  const bool same_episode = nav2_retry_deadline_set_ && nav2_retry_generation_ == generation && nav2_retry_node_index_ == node_index;

  if (nav2_retry_timer_) {
    if (same_episode) 
    {
      return;  
    }
    cancel_nav2_retry();
  }

  if (!same_episode) {
    nav2_retry_generation_ = generation;
    nav2_retry_node_index_ = node_index;
    nav2_retry_deadline_ = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(nav2_dispatch_timeout_sec_));
    nav2_retry_deadline_set_ = true;
  }

  nav2_retry_timer_ = create_wall_timer(
    std::chrono::seconds(2),
    [this]() {
      if (std::chrono::steady_clock::now() >= nav2_retry_deadline_) {
        cancel_nav2_retry();
        nav2_retry_deadline_set_ = false;
        fail_stuck_order("Dispatch did not become possible or kept failing within the "
                          "retry window (Nav2 unavailable, robot pose not valid, or "
                          "repeated navigation failures)");
        return;
      }
      // Re-plans from the current cursor; send_navigation_goal() cancels this timer once a goal goes out.
      dispatch_next_work();
    });
}

// Cancel retry timer if active, no return value.
void BridgeNode::cancel_nav2_retry()
{
  if (nav2_retry_timer_) {
    nav2_retry_timer_->cancel();
    nav2_retry_timer_.reset();
  }
}

// Publish driving state (driving) only on change, no return value.
void BridgeNode::set_driving(bool driving)
{
  if (last_driving_.has_value() && *last_driving_ == driving) {
    return;
  }

  last_driving_ = driving;
  std_msgs::msg::Bool msg;
  msg.data = driving;
  driving_pub_->publish(msg);
}

// Publish paused state (paused) only on change, no return value.
void BridgeNode::set_paused(bool paused)
{
  if (last_paused_.has_value() && *last_paused_ == paused) {
    return;
  }

  last_paused_ = paused;
  std_msgs::msg::Bool msg;
  msg.data = paused;
  paused_pub_->publish(msg);
}

// Write order state (order_id, cursor, terminal) to file, no return value.
void BridgeNode::persist_order_state(
  const std::string& order_id, std::size_t cursor, bool terminal)
{
  try {
    const std::filesystem::path path(order_state_path_);
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(order_state_path_, std::ios::trunc);
    if (!out) {
      RCLCPP_WARN(get_logger(), "Could not open %s to persist order state", order_state_path_.c_str());
      return;
    }
    out << order_id << '\n' << cursor << '\n' << (terminal ? 1 : 0) << '\n';
  } catch (const std::exception& e) {
    RCLCPP_WARN(get_logger(), "Failed to persist order state: %s", e.what());
  }
}

// Fail active order with reason (reason), publish error, persist as complete, no return.
void BridgeNode::fail_stuck_order(const std::string& reason)
{
  if (!order_session_.has_order()) {
    return;
  }
  const auto order_id = order_session_.order_id();
  const auto cursor = order_session_.current_node_index();
  RCLCPP_ERROR(get_logger(), "%s — failing order %s", reason.c_str(), order_id.c_str());
  publish_navigation_error(reason);
  order_session_.clear();
  state_machine_.on_navigation_failed();
  publish_bridge_status();
  persist_order_state(order_id, cursor, true);
  notify_order_dropped(order_id);
}

// Tell the adapter order (order_id) was dropped outside the normal cancelOrder flow, so its
// own OrderManager clears remaining_base_nodes_/order_active_ instead of going stale.
void BridgeNode::notify_order_dropped(const std::string& order_id)
{
  std_msgs::msg::String msg;
  msg.data = order_id;
  order_dropped_pub_->publish(msg);
}

// Read order state from file into (order_id, cursor, terminal); return false if absent/broken.
bool BridgeNode::load_order_state(
  std::string& order_id, std::size_t& cursor, bool& terminal) const
{
  std::ifstream in(order_state_path_);
  if (!in || !std::getline(in, order_id) || order_id.empty()) {
    return false;
  }
  std::size_t terminal_flag = 0;
  if (!(in >> cursor) || !(in >> terminal_flag)) {
    return false;
  }
  terminal = (terminal_flag != 0);
  return true;
}

}  // namespace tb3_vda5050_bridge

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<tb3_vda5050_bridge::BridgeNode>());
  rclcpp::shutdown();
  return 0;
}
