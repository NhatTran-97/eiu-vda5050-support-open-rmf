#include "tb3_vda5050_bridge/bridge_node.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

namespace tb3_vda5050_bridge {

BridgeNode::BridgeNode(const rclcpp::NodeOptions& options)
: rclcpp::Node("tb3_vda5050_bridge", options)
{
  map_id_ = declare_parameter<std::string>("map_id", "map");

  nav2_frame_id_ = declare_parameter<std::string>("nav2_frame_id", "map");
  position_covariance_threshold_ =
    declare_parameter<double>("position_covariance_threshold", 0.5);
  adapter_ns_ = declare_parameter<std::string>("adapter_ns", "/vda5050_client_adapter");
  odom_topic_ = declare_parameter<std::string>("odom_topic", "/odom");
  amcl_pose_topic_ = declare_parameter<std::string>("amcl_pose_topic", "/amcl_pose");
  battery_topic_ = declare_parameter<std::string>("battery_topic", "/battery_state");
  nav2_action_name_ = declare_parameter<std::string>("nav2_action_name", "navigate_to_pose");

  supported_action_types_ = declare_parameter<std::vector<std::string>>(
    "supported_action_types", std::vector<std::string>{});

  using std::placeholders::_1;

  odom_sub_ = create_subscription<nav_msgs::msg::Odometry>(
    odom_topic_, rclcpp::QoS(10),
    std::bind(&BridgeNode::on_odom, this, _1));

  amcl_pose_sub_ = create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(
    amcl_pose_topic_, rclcpp::QoS(10).transient_local(),
    std::bind(&BridgeNode::on_amcl_pose, this, _1));

  battery_sub_ = create_subscription<sensor_msgs::msg::BatteryState>(
    battery_topic_, rclcpp::QoS(10),
    std::bind(&BridgeNode::on_battery, this, _1));

  // transient_local (latched) matches the client_adapter's order publisher so a
  // late-subscribing or restarted bridge still receives the latest order, instead
  // of silently missing it and leaving the client's order lifecycle deadlocked.
  order_sub_ = create_subscription<vda5050_msgs::msg::Order>(
    adapter_topic("order"), rclcpp::QoS(10).transient_local(),
    std::bind(&BridgeNode::on_order, this, _1));

  action_cancel_sub_ = create_subscription<std_msgs::msg::String>(
    adapter_topic("action_cancel"), rclcpp::QoS(10),
    std::bind(&BridgeNode::on_action_cancel, this, _1));

  action_execute_sub_ = create_subscription<vda5050_msgs::msg::Action>(
    adapter_topic("action_execute"), rclcpp::QoS(10),
    std::bind(&BridgeNode::on_action_execute, this, _1));

  agv_position_pub_ = create_publisher<vda5050_msgs::msg::AgvPosition>(
    adapter_topic("agv_position"), rclcpp::QoS(10));
  battery_state_pub_ = create_publisher<vda5050_msgs::msg::BatteryState>(
    adapter_topic("battery_state"), rclcpp::QoS(10));
  velocity_pub_ = create_publisher<vda5050_msgs::msg::Velocity>(
    adapter_topic("velocity"), rclcpp::QoS(10));
  driving_pub_ = create_publisher<std_msgs::msg::Bool>(
    adapter_topic("driving"), rclcpp::QoS(10));
  paused_pub_ = create_publisher<std_msgs::msg::Bool>(
    adapter_topic("paused"), rclcpp::QoS(10));
  node_reached_pub_ = create_publisher<vda5050_msgs::msg::NodeState>(
    adapter_topic("node_reached"), rclcpp::QoS(10));
  edge_entered_pub_ = create_publisher<vda5050_msgs::msg::EdgeState>(
    adapter_topic("edge_entered"), rclcpp::QoS(10));
  edge_completed_pub_ = create_publisher<vda5050_msgs::msg::EdgeState>(
    adapter_topic("edge_completed"), rclcpp::QoS(10));
  action_state_feedback_pub_ = create_publisher<vda5050_msgs::msg::ActionState>(
    adapter_topic("action_state_feedback"), rclcpp::QoS(10));
  error_pub_ = create_publisher<vda5050_msgs::msg::Error>(
    adapter_topic("error"), rclcpp::QoS(10));

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

void BridgeNode::on_odom(const nav_msgs::msg::Odometry::SharedPtr msg)
{
  // Odom is used only for velocity — position comes from AMCL (map frame).
  vda5050_msgs::msg::Velocity vel;
  vel.vx = msg->twist.twist.linear.x;
  vel.vy = 0.0;
  vel.omega = msg->twist.twist.angular.z;
  velocity_pub_->publish(vel);
}

void BridgeNode::on_amcl_pose(
  const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
{
  tf2::Quaternion q;
  tf2::fromMsg(msg->pose.pose.orientation, q);
  tf2::Matrix3x3 m(q);
  double roll = 0.0, pitch = 0.0, yaw = 0.0;
  m.getRPY(roll, pitch, yaw);

  robot_x_   = msg->pose.pose.position.x;
  robot_y_   = msg->pose.pose.position.y;
  robot_yaw_ = yaw;
  robot_pose_valid_ = true;

  vda5050_msgs::msg::AgvPosition pos;
  pos.x = robot_x_;
  pos.y = robot_y_;
  pos.theta = robot_yaw_;
  pos.map_id = map_id_;
  pos.position_initialized =
    (msg->pose.covariance[0] >= 0.0 &&
     msg->pose.covariance[0] < position_covariance_threshold_);
  pos.localization_score = -1.0;
  pos.deviation_range = -1.0;
  agv_position_pub_->publish(pos);
}

void BridgeNode::on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg)
{
  vda5050_msgs::msg::BatteryState batt;

  // TB3 OpenCR publishes percentage in 0–100 scale (non-standard).
  // Standard ROS sensor_msgs uses 0.0–1.0, so detect and handle both.
  const float pct = msg->percentage;
  float battery_charge;
  if (pct > 1.0f) {
    // TB3 style: already 0–100
    battery_charge = std::clamp(pct, 0.0f, 100.0f);
  } else if (!std::isnan(pct) && pct >= 0.0f) {
    // Standard ROS 0.0–1.0
    battery_charge = pct * 100.0f;
  } else {
    // Derive from voltage (3S LiPo: 12.6V full → 9.0V empty)
    constexpr float V_FULL = 12.6f, V_EMPTY = 9.0f;
    const float v = msg->voltage;
    battery_charge = (v > 1.0f) ?
      std::clamp((v - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f, 0.0f, 100.0f) : 100.0f;
  }
  batt.battery_charge = battery_charge;   // VDA5050: 0–100 %
  batt.battery_voltage = msg->voltage;
  batt.battery_health = -1;
  batt.charging =
    (msg->power_supply_status ==
     sensor_msgs::msg::BatteryState::POWER_SUPPLY_STATUS_CHARGING);
  batt.reach = 0;
  battery_state_pub_->publish(batt);
}

void BridgeNode::on_order(const vda5050_msgs::msg::Order::SharedPtr msg)
{
  const bool is_update =
    order_session_.has_order() && order_session_.order_id() == msg->order_id;

  if (is_update) {

    RCLCPP_INFO(
      get_logger(),
      "Order update: id=%s updateId=%u nodes=%zu",
      msg->order_id.c_str(),
      msg->order_update_id,
      msg->nodes.size());

    order_session_.update(*msg);

  
    const auto mode = state_machine_.mode();
    if (mode == BridgeMode::WAITING_FOR_RELEASE || mode == BridgeMode::IDLE) {
      dispatch_next_work();
    }
    return;
  }

  RCLCPP_INFO(
    get_logger(),
    "Order received: id=%s nodes=%zu",
    msg->order_id.c_str(),
    msg->nodes.size());

  cancel_navigation();
  order_session_.start(*msg);
  state_machine_.on_order_started();
  publish_bridge_status();
  dispatch_next_work();
}

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
    cancel_navigation();
    order_session_.clear();
    state_machine_.on_cancel_requested();
    publish_bridge_status();
    return;
  }

  RCLCPP_WARN(get_logger(), "Unknown action_cancel payload: %s", data.c_str());
}

void BridgeNode::on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg)
{
  RCLCPP_INFO(
    get_logger(),
    "Action execute: id=%s type=%s",
    msg->action_id.c_str(),
    msg->action_type.c_str());

  publish_action_feedback(*msg, "RUNNING");

  const bool supported =
    std::find(supported_action_types_.begin(), supported_action_types_.end(),
              msg->action_type) != supported_action_types_.end();

  if (!supported) {

    RCLCPP_WARN(
      get_logger(),
      "Unsupported action type '%s' (id=%s) — reporting FAILED",
      msg->action_type.c_str(),
      msg->action_id.c_str());
    publish_action_feedback(
      *msg, "FAILED",
      "Action type '" + msg->action_type + "' not supported by tb3_vda5050_bridge");
    return;
  }


  publish_action_feedback(*msg, "FINISHED", "Completed (no-op handler)");
}

void BridgeNode::dispatch_next_work()
{
  if (state_machine_.is_paused()) {
    return;
  }


  while (true) {
    const auto plan = order_session_.plan_next_work();
    publish_traversal_events(plan.immediate_events);

    switch (plan.kind) {
      case DispatchKind::NAVIGATE:
        if (try_complete_in_place(*plan.target)) {
          continue;  // node reached without Nav2; advance to the next one
        }
        state_machine_.on_dispatching();
        publish_bridge_status();
        send_navigation_goal(*plan.target);
        return;

      case DispatchKind::WAITING_FOR_RELEASE:
        state_machine_.on_waiting_for_release();
        publish_bridge_status();
        RCLCPP_INFO(
          get_logger(),
          "Order %s is waiting for more released nodes at index %zu",
          order_session_.order_id().c_str(),
          order_session_.current_node_index());
        return;

      case DispatchKind::COMPLETED:
      default:
        state_machine_.on_all_work_completed();
        publish_bridge_status();
        if (order_session_.has_order()) {
          RCLCPP_INFO(
            get_logger(),
            "All work completed for order %s",
            order_session_.order_id().c_str());
        }
        return;
    }
  }
}

bool BridgeNode::try_complete_in_place(const NavigationTarget& target)
{
  // Skip Nav2 for trivial navigation: robot already within tolerance of target.
  // Avoids unnecessary rotation when holding-in-place (same start and end).
  if (!robot_pose_valid_) {
    return false;
  }

  const double dx = target.node.node_position.x - robot_x_;
  const double dy = target.node.node_position.y - robot_y_;
  const double dist_sq = dx * dx + dy * dy;
  const double xy_tol = target.node.node_position.allowed_deviation_xy > 0.0
                        ? target.node.node_position.allowed_deviation_xy : 0.5;
  if (dist_sq >= xy_tol * xy_tol) {
    return false;
  }

  RCLCPP_INFO(
    get_logger(),
    "Skipping Nav2 for node %s — robot already within %.2fm of target",
    target.node.node_id.c_str(), std::sqrt(dist_sq));
  if (target.incoming_edge.has_value()) {
    edge_entered_pub_->publish(*target.incoming_edge);
  }
  publish_traversal_events(order_session_.complete_navigation(target.node_index));
  return true;
}

void BridgeNode::send_navigation_goal(const NavigationTarget& target)
{
  // 10s (not 2s): with latched order QoS the bridge can receive an order the
  // instant it starts, before its action client has discovered the Nav2 server.
  // A short timeout then spuriously reports "action server not available".
  if (!nav2_client_->wait_for_action_server(std::chrono::seconds(10))) {
    state_machine_.on_navigation_failed();
    publish_bridge_status();
    publish_navigation_error("Nav2 action server not available", target.node.node_id);
    return;
  }

  if (target.incoming_edge.has_value()) {
    edge_entered_pub_->publish(*target.incoming_edge);
  }

  NavigateToPose::Goal goal;

  goal.pose.header.frame_id = nav2_frame_id_;
  goal.pose.header.stamp = now();
  goal.pose.pose.position.x = target.node.node_position.x;
  goal.pose.pose.position.y = target.node.node_position.y;

  tf2::Quaternion q;
  q.setRPY(0.0, 0.0, target.node.node_position.theta);
  goal.pose.pose.orientation = tf2::toMsg(q);

  const uint64_t token = ++navigation_token_;
  const uint64_t generation = order_session_.generation();
  const std::size_t node_index = target.node_index;
  const std::string node_id = target.node.node_id;

  RCLCPP_INFO(
    get_logger(),
    "Dispatching node %s (seq=%u) with token=%llu",
    target.node.node_id.c_str(),
    target.node.sequence_id,
    static_cast<unsigned long long>(token));

  auto send_goal_options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();

  send_goal_options.goal_response_callback =
    [this, token, generation, node_id](const GoalHandle::SharedPtr& handle) {
      if (token != navigation_token_ || generation != order_session_.generation()) {
        return;
      }

      if (!handle) {
        state_machine_.on_navigation_failed();
        publish_bridge_status();
        publish_navigation_error("Nav2 goal rejected", node_id);
        return;
      }

      current_goal_handle_ = handle;
      state_machine_.on_navigation_active();
      publish_bridge_status();
      RCLCPP_INFO(
        get_logger(),
        "Nav2 goal accepted for node %s (token=%llu)",
        node_id.c_str(),
        static_cast<unsigned long long>(token));
    };

  send_goal_options.result_callback =
    [this, token, generation, node_index, node_id](const GoalHandle::WrappedResult& result) {
      if (token != navigation_token_ || generation != order_session_.generation()) {
        RCLCPP_DEBUG(
          get_logger(),
          "Ignoring stale Nav2 result for node %s (token=%llu)",
          node_id.c_str(),
          static_cast<unsigned long long>(token));
        return;
      }

      current_goal_handle_.reset();

      if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
        publish_traversal_events(order_session_.complete_navigation(node_index));
        dispatch_next_work();
        return;
      }

      state_machine_.on_navigation_failed();
      publish_bridge_status();
      publish_navigation_error("Navigation goal failed or was cancelled", node_id);
    };

  nav2_client_->async_send_goal(goal, send_goal_options);
}

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
      node_reached_pub_->publish(*event.node_reached);
    }
  }
}

void BridgeNode::publish_action_feedback(const vda5050_msgs::msg::Action& action,
                                         const std::string& status,
                                         const std::string& description)
{
  vda5050_msgs::msg::ActionState state;
  state.action_id = action.action_id;
  state.action_type = action.action_type;
  state.action_description = action.action_description;
  state.action_status = status;
  state.result_description = description;
  action_state_feedback_pub_->publish(state);
}

void BridgeNode::publish_navigation_error(const std::string& description,
                                          const std::string& node_id)
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

void BridgeNode::publish_bridge_status()
{
  const auto status = state_machine_.status();
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

void BridgeNode::invalidate_navigation_context()
{
  ++navigation_token_;
  current_goal_handle_.reset();
}

void BridgeNode::set_driving(bool driving)
{
  if (last_driving_ == driving) {
    return;
  }

  last_driving_ = driving;
  std_msgs::msg::Bool msg;
  msg.data = driving;
  driving_pub_->publish(msg);
}

void BridgeNode::set_paused(bool paused)
{
  if (last_paused_ == paused) {
    return;
  }

  last_paused_ = paused;
  std_msgs::msg::Bool msg;
  msg.data = paused;
  paused_pub_->publish(msg);
}

}  // namespace tb3_vda5050_bridge

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<tb3_vda5050_bridge::BridgeNode>());
  rclcpp::shutdown();
  return 0;
}
