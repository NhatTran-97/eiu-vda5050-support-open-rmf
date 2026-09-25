#include "tb3_vda5050_bridge/bridge_node.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <utility>

#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

namespace tb3_vda5050_bridge {

namespace {

// Wrap angle into (-pi, pi].
double normalize_angle(double angle)
{
  while (angle > M_PI) angle -= 2.0 * M_PI;
  while (angle < -M_PI) angle += 2.0 * M_PI;
  return angle;
}

// Throw std::invalid_argument naming the parameter (name) unless ok.
void require(bool ok, const std::string& name, const std::string& rule)
{
  if (!ok) throw std::invalid_argument("Parameter '" + name + "' must be " + rule);
}

// Value of an action parameter, or empty if absent.
std::string find_action_parameter(const vda5050_msgs::msg::Action& action, const std::string& key)
{
  for (const auto& parameter : action.action_parameters) 
  {
    if (parameter.key == key) return parameter.value;
  }
  return "";
}

// Random 64-bit value as 16 hex digits.
std::string make_session_id()
{
  std::random_device device;
  std::mt19937_64 engine(device());
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(engine()));
  return buffer;
}

}  // namespace

BridgeNode::BridgeNode(const rclcpp::NodeOptions& options): rclcpp::Node("tb3_vda5050_bridge", options), session_id_(make_session_id())
{
  map_id_ = declare_parameter<std::string>("map_id", "map");
  nav2_frame_id_ = declare_parameter<std::string>("nav2_frame_id", "map");
  position_covariance_threshold_ = declare_parameter<double>("position_covariance_threshold", 0.5);
  adapter_ns_ = declare_parameter<std::string>("adapter_ns", "/vda5050_client_adapter");
  odom_topic_ = declare_parameter<std::string>("odom_topic", "/odom");
  amcl_pose_topic_ = declare_parameter<std::string>("amcl_pose_topic", "/amcl_pose");
  initial_pose_topic_ = declare_parameter<std::string>("initial_pose_topic", "/initialpose");
  speed_limit_topic_ = declare_parameter<std::string>("speed_limit_topic", "/speed_limit");
  battery_topic_ = declare_parameter<std::string>("battery_topic", "/battery_state");
  nav2_action_name_ = declare_parameter<std::string>("nav2_action_name", "navigate_to_pose");
  supported_action_types_ = declare_parameter<std::vector<std::string>>("supported_action_types", std::vector<std::string>{});
  nav2_dispatch_timeout_sec_ = declare_parameter<double>("nav2_dispatch_timeout_sec", 120.0);
  amcl_pose_timeout_sec_ = declare_parameter<double>("amcl_pose_timeout_sec", 10.0);
  pose_stale_move_tolerance_m_ = declare_parameter<double>("pose_stale_move_tolerance_m", 0.15);
  nav2_retry_period_sec_ = declare_parameter<double>("nav2_retry_period_sec", 2.0);
  default_allowed_deviation_xy_ = declare_parameter<double>("default_allowed_deviation_xy", 0.5);
  unconstrained_theta_rad_ = declare_parameter<double>("unconstrained_theta_rad", 3.0);
  battery_voltage_full_ = declare_parameter<double>("battery_voltage_full", 12.6);
  battery_voltage_empty_ = declare_parameter<double>("battery_voltage_empty", 9.0);
  initial_pose_covariance_xy_ = declare_parameter<double>("initial_pose_covariance_xy", 0.25);
  initial_pose_covariance_yaw_ = declare_parameter<double>("initial_pose_covariance_yaw", 0.06853891945200942);
  odom_publish_min_interval_sec_ = declare_parameter<double>("odom_publish_min_interval_sec", 0.1);
  driver_status_lease_sec_ = declare_parameter<double>("driver_status_lease_sec", 1.0);
  diagnostics_topic_ = declare_parameter<std::string>("diagnostics_topic", "/diagnostics");
  twist_mux_status_name_ = declare_parameter<std::string>("twist_mux_status_name", "twist_mux: Twist mux status");
  navigation_velocity_source_ = declare_parameter<std::string>("navigation_velocity_source", "navigation");

  require(position_covariance_threshold_ > 0.0, "position_covariance_threshold", "> 0");
  require(nav2_dispatch_timeout_sec_ > 0.0, "nav2_dispatch_timeout_sec", "> 0");
  require(nav2_retry_period_sec_ > 0.0, "nav2_retry_period_sec", "> 0");
  require(amcl_pose_timeout_sec_ > 0.0, "amcl_pose_timeout_sec", "> 0");
  require(pose_stale_move_tolerance_m_ >= 0.0, "pose_stale_move_tolerance_m", ">= 0");
  require(default_allowed_deviation_xy_ > 0.0, "default_allowed_deviation_xy", "> 0");
  require(unconstrained_theta_rad_ > 0.0, "unconstrained_theta_rad", "> 0");
  require(battery_voltage_full_ > battery_voltage_empty_, "battery_voltage_full", "> battery_voltage_empty");
  require(initial_pose_covariance_xy_ > 0.0 && initial_pose_covariance_yaw_ > 0.0,"initial_pose_covariance_xy/yaw", "> 0");
  require(odom_publish_min_interval_sec_ >= 0.0, "odom_publish_min_interval_sec", ">= 0");
  require(driver_status_lease_sec_ > 0.0, "driver_status_lease_sec", "> 0");
  using std::placeholders::_1;
  using std::placeholders::_2;

  odom_sub_ = create_subscription<nav_msgs::msg::Odometry>(odom_topic_, rclcpp::QoS(10), std::bind(&BridgeNode::on_odom, this, _1));
  amcl_pose_sub_ = create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>(amcl_pose_topic_, rclcpp::QoS(10).transient_local(), std::bind(&BridgeNode::on_amcl_pose, this, _1));
  battery_sub_ = create_subscription<sensor_msgs::msg::BatteryState>(battery_topic_, rclcpp::QoS(10), std::bind(&BridgeNode::on_battery, this, _1));
  action_execute_sub_ = create_subscription<vda5050_msgs::msg::Action>(adapter_topic("action_execute"), rclcpp::QoS(10), std::bind(&BridgeNode::on_action_execute, this, _1));
  action_command_sub_ = create_subscription<vda5050_msgs::msg::ActionCommand>(adapter_topic("action_command"), rclcpp::QoS(10),[this](vda5050_msgs::msg::ActionCommand::SharedPtr msg) {
      // Actions here complete synchronously in on_action_execute().
      RCLCPP_DEBUG(get_logger(), "Action command %u for %s ignored", msg->command, msg->action_id.c_str());
    });
  local_command_sub_ = create_subscription<std_msgs::msg::String>(adapter_topic("action_cancel"), rclcpp::QoS(10), std::bind(&BridgeNode::on_local_command, this, _1));
  diagnostics_sub_ = create_subscription<diagnostic_msgs::msg::DiagnosticArray>(diagnostics_topic_, rclcpp::QoS(10), std::bind(&BridgeNode::on_diagnostics, this, _1));

  initial_pose_pub_ = create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(initial_pose_topic_, rclcpp::QoS(10));
  speed_limit_pub_ = create_publisher<nav2_msgs::msg::SpeedLimit>(speed_limit_topic_, rclcpp::QoS(10));
  agv_position_pub_ = create_publisher<vda5050_msgs::msg::AgvPosition>(adapter_topic("agv_position"), rclcpp::QoS(10));
  battery_state_pub_ = create_publisher<vda5050_msgs::msg::BatteryState>(adapter_topic("battery_state"), rclcpp::QoS(10));
  velocity_pub_ = create_publisher<vda5050_msgs::msg::Velocity>(adapter_topic("velocity"), rclcpp::QoS(10));

  // Manual liveliness: a stalled executor stops the heartbeat and the adapter sees the driver lost.
  const auto lease = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::duration<double>(driver_status_lease_sec_));
  driver_status_pub_ = create_publisher<vda5050_msgs::msg::DriverStatus>(adapter_topic("driver_status"),rclcpp::QoS(1).transient_local() .liveliness(rclcpp::LivelinessPolicy::ManualByTopic).liveliness_lease_duration(rclcpp::Duration(lease)));
  action_state_feedback_pub_ = create_publisher<vda5050_msgs::msg::ActionState>(adapter_topic("action_state_feedback"), rclcpp::QoS(10));
  distance_since_last_node_pub_ = create_publisher<std_msgs::msg::Float64>(adapter_topic("distance_since_last_node"), rclcpp::QoS(10));
  operating_mode_pub_ = create_publisher<std_msgs::msg::String>(adapter_topic("operating_mode"), rclcpp::QoS(1).transient_local());

  nav2_client_ = rclcpp_action::create_client<NavigateToPose>(this, nav2_action_name_);
  step_server_ = rclcpp_action::create_server<NavigateToNode>(this, adapter_topic("navigate_to_node"), std::bind(&BridgeNode::on_step_goal, this, _1, _2),
  std::bind(&BridgeNode::on_step_cancel, this, _1), std::bind(&BridgeNode::on_step_accepted, this, _1));
  cancel_check_timer_ = create_wall_timer(std::chrono::milliseconds(20), [this]() { check_step_cancel(); });
  driver_status_timer_ = create_wall_timer(lease / 3, [this]() { publish_driver_status(); });

  publish_driver_status();
  RCLCPP_INFO(get_logger(),"TB3 VDA5050 Bridge started - map_id=%s nav2_frame_id=%s adapter_ns=%s nav2_action=%s session=%s",
    map_id_.c_str(), nav2_frame_id_.c_str(), adapter_ns_.c_str(), nav2_action_name_.c_str(), session_id_.c_str());
}

// ─── Telemetry ───────────────────────────────────────────────────────────────

// Accumulates the distance driven; publishes velocity and leg distance at most every odom_publish_min_interval_sec.
void BridgeNode::on_odom(const nav_msgs::msg::Odometry::SharedPtr msg)
{
  const double odom_x = msg->pose.pose.position.x;
  const double odom_y = msg->pose.pose.position.y;
  odom_distance_tracker_.update(odom_x, odom_y);
  last_odom_x_ = odom_x;
  last_odom_y_ = odom_y;
  last_odom_position_valid_ = true;

  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - last_odom_publish_).count() < odom_publish_min_interval_sec_) 
  {
    return;
  }
  last_odom_publish_ = now;

  vda5050_msgs::msg::Velocity vel;
  vel.vx = msg->twist.twist.linear.x;
  vel.vy = 0.0;
  vel.omega = msg->twist.twist.angular.z;
  velocity_pub_->publish(vel);

  std_msgs::msg::Float64 dist_msg;
  dist_msg.data = odom_distance_tracker_.current();
  distance_since_last_node_pub_->publish(dist_msg);
}

// Caches the AMCL pose, checks its covariance and publishes the position.
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

  // Reject non-finite poses while retaining the last valid coordinates.
  if (finite) {
    robot_x_ = x;
    robot_y_ = y;
    robot_yaw_ = yaw;
  }

  // Record odometry at the last confident AMCL pose.
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
  if (elapsed < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(amcl_pose_timeout_sec_))) 
  {
    return true;
  }
  // Accept an older AMCL pose when odometry shows the robot stayed still.
  if (!has_driven_since_last_amcl_pose_) 
  {
    return true;
  }
  if (!odom_at_last_amcl_pose_valid_ || !last_odom_position_valid_) 
  {
    return false;
  }
  const double moved_since = std::hypot(last_odom_x_ - odom_x_at_last_amcl_pose_, last_odom_y_ - odom_y_at_last_amcl_pose_);
  return moved_since < pose_stale_move_tolerance_m_;
}

// Publishes the battery charge as 0-100 %.
void BridgeNode::on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg)
{
  vda5050_msgs::msg::BatteryState batt;

  // TB3 OpenCR reports 0-100, standard ROS 0-1.
  const float pct = msg->percentage;
  float battery_charge;
  bool have_reading = true;
  if (pct > 1.0f) 
  {
    battery_charge = std::clamp(pct, 0.0f, 100.0f);
  } else if (!std::isnan(pct) && pct >= 0.0f) 
  {
    battery_charge = pct * 100.0f;
  } else if (msg->voltage > 1.0f) 
  {
    const double fraction = (msg->voltage - battery_voltage_empty_) / (battery_voltage_full_ - battery_voltage_empty_);
    battery_charge = static_cast<float>(std::clamp(fraction * 100.0, 0.0, 100.0));
  } else 
  {
    have_reading = false;
    battery_charge = last_battery_charge_;
    if (!last_battery_valid_) 
    {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 30000, "Battery percentage and voltage both invalid, no prior reading — not publishing battery_state yet");
      return;
    }
  }
  if (have_reading) {
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

// An unmasked non-navigation velocity source means manual control is active.
void BridgeNode::on_diagnostics(const diagnostic_msgs::msg::DiagnosticArray::SharedPtr msg)
{
  static const std::string prefix = "velocity topics.";
  for (const auto& status : msg->status) {
    if (status.name != twist_mux_status_name_) continue;

    bool override_active = false;
    for (const auto& kv : status.values) 
    {
      if (kv.key.rfind(prefix, 0) != 0 || kv.key == prefix + navigation_velocity_source_) continue;
      if (kv.value.find("unmasked") != std::string::npos) {
        override_active = true;
        break;
      }
    }

    const std::string mode = override_active ? "MANUAL" : "AUTOMATIC";
    if (mode == last_operating_mode_) return;
    last_operating_mode_ = mode;

    std_msgs::msg::String out;
    out.data = mode;
    operating_mode_pub_->publish(out);
    RCLCPP_INFO(get_logger(), "Operating mode -> %s", mode.c_str());
    return;
  }
}

// ─── Actions ─────────────────────────────────────────────────────────────────

// Runs an instant action and reports its result.
void BridgeNode::on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg)
{
  RCLCPP_INFO(get_logger(), "Action execute: id=%s type=%s", msg->action_id.c_str(), msg->action_type.c_str());

  publish_action_feedback(*msg, "RUNNING");

  const bool supported = std::find(supported_action_types_.begin(), supported_action_types_.end(),
                                   msg->action_type) != supported_action_types_.end();
  if (!supported) 
  {
    RCLCPP_WARN(get_logger(), "Unsupported action type '%s' (id=%s) — reporting FAILED",msg->action_type.c_str(), msg->action_id.c_str());
    publish_action_feedback(*msg, "FAILED", "Action type '" + msg->action_type + "' not supported by tb3_vda5050_bridge");
    return;
  }

  if (msg->action_type == "initPosition") 
  {
    init_position(*msg);
    return;
  }

  publish_action_feedback(*msg, "FINISHED", "Completed (no-op handler)");
}

// Sets the AMCL initial pose from the action's x, y, theta; refused while a goal drives from a valid pose.
void BridgeNode::init_position(const vda5050_msgs::msg::Action& action)
{
  if (current_goal_handle_ && robot_pose_confident_) 
  {
    RCLCPP_WARN(get_logger(), "initPosition (id=%s) rejected: robot is executing a navigation goal", action.action_id.c_str());
    publish_action_feedback(action, "FAILED", "initPosition refused while the robot is navigating with a valid pose — " "cancel or finish the current order first");
    return;
  }

  double x = 0.0, y = 0.0, theta = 0.0;
  try {
    x = std::stod(find_action_parameter(action, "x"));
    y = std::stod(find_action_parameter(action, "y"));
    theta = std::stod(find_action_parameter(action, "theta"));
  } catch (const std::exception&) 
  {
    RCLCPP_WARN(get_logger(), "initPosition (id=%s) missing/invalid numeric x/y/theta parameters",action.action_id.c_str());
    publish_action_feedback(action, "FAILED", "initPosition requires numeric x, y, theta parameters");
    return;
  }

  // The active step was planned from the previous pose.
  if (active_step_) 
  {
    cancel_navigation();
    finish_step(active_step_, NavigateToNode::Result::DROPPED, "start pose invalidated by initPosition");
    set_driving(false);
  }

  geometry_msgs::msg::PoseWithCovarianceStamped pose;
  pose.header.frame_id = nav2_frame_id_;
  pose.header.stamp = now();
  pose.pose.pose.position.x = x;
  pose.pose.pose.position.y = y;
  tf2::Quaternion q;
  q.setRPY(0.0, 0.0, theta);
  pose.pose.pose.orientation = tf2::toMsg(q);
  pose.pose.covariance[6 * 0 + 0] = initial_pose_covariance_xy_;
  pose.pose.covariance[6 * 1 + 1] = initial_pose_covariance_xy_;
  pose.pose.covariance[6 * 5 + 5] = initial_pose_covariance_yaw_;

  initial_pose_pub_->publish(pose);
  RCLCPP_INFO(get_logger(), "initPosition (id=%s): published initial pose (%.2f, %.2f, %.2f rad) to %s", action.action_id.c_str(), x, y, theta, initial_pose_topic_.c_str());
  publish_action_feedback(action, "FINISHED", "Initial pose published to AMCL");
}

// "cancel:*" from local tools (e.g. robot_local_ui) drops the active step; other commands go through the adapter.
void BridgeNode::on_local_command(const std_msgs::msg::String::SharedPtr msg)
{
  if (msg->data.rfind("cancel:", 0) != 0) 
  {
    RCLCPP_WARN(get_logger(), "Local command '%s' not supported; send it through the client adapter", msg->data.c_str());
    return;
  }
  RCLCPP_INFO(get_logger(), "Local cancel received");
  cancel_navigation();
  if (active_step_) 
  {
    finish_step(active_step_, NavigateToNode::Result::DROPPED, "cancelled on the robot");
  }
  set_driving(false);
}

// ─── NavigateToNode steps ────────────────────────────────────────────────────

rclcpp_action::GoalResponse BridgeNode::on_step_goal(const rclcpp_action::GoalUUID&,
                                                     std::shared_ptr<const NavigateToNode::Goal> goal)
{
  RCLCPP_INFO(get_logger(), "Step received: order=%s update=%u node=%s (seq=%u)", goal->order_id.c_str(), goal->order_update_id, goal->node.node_id.c_str(), goal->node.sequence_id);
  return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse BridgeNode::on_step_cancel(const std::shared_ptr<StepHandle>)
{
  return rclcpp_action::CancelResponse::ACCEPT;
}

// A new step replaces the active one; Nav2 preempts its goal with the new one.
void BridgeNode::on_step_accepted(const std::shared_ptr<StepHandle> handle)
{
  if (active_step_) 
  {
    finish_step(active_step_, NavigateToNode::Result::PREEMPTED, "replaced by a newer step");
  }
  const auto goal = handle->get_goal();
  if (goal->order_id != last_step_order_id_) 
  {
    odom_distance_tracker_.take();
    last_step_order_id_ = goal->order_id;
  }
  active_step_ = handle;
  ++step_token_;
  auto previous_goal = current_goal_handle_;
  current_goal_handle_.reset();
  goal_sent_ = false;
  run_step();
  if (previous_goal && !goal_sent_) 
  {
    nav2_client_->async_cancel_goal(previous_goal);
  }
}

// Stops navigation and finishes the active step once its client asked to cancel it.
void BridgeNode::check_step_cancel()
{
  if (!active_step_ || !active_step_->is_canceling()) 
  {
    return;
  }
  cancel_navigation();
  finish_step(active_step_, NavigateToNode::Result::CANCELED, "cancelled by the client");
  set_driving(false);
}

// Completes the step in place, sends its Nav2 goal, or waits for a valid pose.
void BridgeNode::run_step()
{
  if (!active_step_) 
  {
    return;
  }
  const auto& node = active_step_->get_goal()->node;

  if (!node.node_position_set) 
  {
    if (!robot_pose_valid()) 
    {
      RCLCPP_WARN(get_logger(), "Robot pose not valid/confident — holding position-less node %s, will retry", node.node_id.c_str());
      arm_nav2_retry();
      return;
    }
    finish_step(active_step_, NavigateToNode::Result::REACHED, "position-less node");
    return;
  }

  if (at_node(node)) 
  {
    RCLCPP_INFO(get_logger(), "Skipping Nav2 for node %s — robot already at the target", node.node_id.c_str());
    finish_step(active_step_, NavigateToNode::Result::REACHED, "already at the node");
    set_driving(false);
    return;
  }
  send_navigation_goal();
}

// Finishes `handle` with `outcome` and, when reached, the distance driven since the previous node.
void BridgeNode::finish_step(std::shared_ptr<StepHandle> handle, uint8_t outcome, const std::string& description)
{
  auto result = std::make_shared<NavigateToNode::Result>();
  result->outcome = outcome;
  result->description = description;
  if (outcome == NavigateToNode::Result::REACHED) 
  {
    result->distance_driven = odom_distance_tracker_.take();
  }

  if (handle->is_active()) 
  {
    if (outcome == NavigateToNode::Result::REACHED) 
    {
      handle->succeed(result);
    } else if (outcome == NavigateToNode::Result::CANCELED && handle->is_canceling()) 
    {
      handle->canceled(result);
    } else 
    {
      handle->abort(result);
    }
  }
  if (handle == active_step_) 
  {
    active_step_.reset();
    reset_nav2_retry();
  }
  RCLCPP_INFO(get_logger(), "Step %s finished: outcome=%u (%s)", handle->get_goal()->node.node_id.c_str(), outcome, description.c_str());
}

// ─── Navigation ──────────────────────────────────────────────────────────────

void BridgeNode::apply_speed_limit(double max_speed)
{
  nav2_msgs::msg::SpeedLimit msg;
  msg.header.stamp = now();
  msg.percentage = false;
  msg.speed_limit = max_speed >= 0.0 ? max_speed : 0.0;
  speed_limit_pub_->publish(msg);
  if (max_speed >= 0.0) 
  {
    RCLCPP_INFO(get_logger(), "Applying edge speed limit: %.3f m/s", max_speed);
  }
}

// Whether the robot is within the node's position (and, when constrained, heading) tolerance on the same map.
bool BridgeNode::at_node(const vda5050_msgs::msg::Node& node) const
{
  if (!robot_pose_valid()) 
  {
    return false;
  }
  if (!node.node_position.map_id.empty() && node.node_position.map_id != map_id_) 
  {
    return false;
  }
  const double dx = node.node_position.x - robot_x_;
  const double dy = node.node_position.y - robot_y_;
  const double xy_tol = node.node_position.allowed_deviation_xy > 0.0 ? node.node_position.allowed_deviation_xy : default_allowed_deviation_xy_;
  if (dx * dx + dy * dy >= xy_tol * xy_tol) 
  {
    return false;
  }
  const bool theta_constrained = node.node_position.theta_set && node.node_position.allowed_deviation_theta < unconstrained_theta_rad_;
  return !theta_constrained || std::fabs(normalize_angle(node.node_position.theta - robot_yaw_)) < node.node_position.allowed_deviation_theta;
}

// Sends the Nav2 goal for the active step, or arms a retry if Nav2 isn't ready yet.
void BridgeNode::send_navigation_goal()
{
  const auto step = active_step_;
  const auto goal_msg = step->get_goal();
  const auto& node = goal_msg->node;

  if (!nav2_client_->action_server_is_ready()) 
  {
    RCLCPP_WARN(get_logger(), "Nav2 action server not available yet — holding node %s, will retry",
                node.node_id.c_str());
    arm_nav2_retry();
    return;
  }
  apply_speed_limit(goal_msg->incoming_edge_set ? goal_msg->incoming_edge.max_speed : -1.0);

  NavigateToPose::Goal goal;
  goal.pose.header.frame_id = nav2_frame_id_;
  goal.pose.header.stamp = now();
  goal.pose.pose.position.x = node.node_position.x;
  goal.pose.pose.position.y = node.node_position.y;

  // With no heading constraint, aim the goal yaw along the bearing to avoid a rotate-in-place.
  const bool theta_constrained = node.node_position.theta_set &&
                                 node.node_position.allowed_deviation_theta < unconstrained_theta_rad_;
  double goal_yaw = node.node_position.theta;
  if (!theta_constrained && robot_pose_valid()) 
  {
    const double dx = node.node_position.x - robot_x_;
    const double dy = node.node_position.y - robot_y_;
    goal_yaw = (std::hypot(dx, dy) > 1e-3) ? std::atan2(dy, dx) : robot_yaw_;
  }
  tf2::Quaternion q;
  q.setRPY(0.0, 0.0, goal_yaw);
  goal.pose.pose.orientation = tf2::toMsg(q);

  const uint64_t token = step_token_;
  const std::string node_id = node.node_id;
  RCLCPP_INFO(get_logger(), "Dispatching node %s (seq=%u)", node_id.c_str(), node.sequence_id);
  goal_sent_ = true;

  auto options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
  options.goal_response_callback = [this, token, step, node_id](const Nav2GoalHandle::SharedPtr& handle) 
  {
    if (token != step_token_ || step != active_step_) 
    {
      if (handle) nav2_client_->async_cancel_goal(handle);
      return;
    }
    if (!handle) 
    {
      // Often transient (AMCL not converged, costmap not ready): retry like an unready server.
      RCLCPP_WARN(get_logger(), "Nav2 rejected the goal for node %s — holding, will retry", node_id.c_str());
      arm_nav2_retry();
      return;
    }
    current_goal_handle_ = handle;
    set_driving(true);
    auto feedback = std::make_shared<NavigateToNode::Feedback>();
    feedback->edge_entered = step->get_goal()->incoming_edge_set;
    step->publish_feedback(feedback);
    RCLCPP_INFO(get_logger(), "Nav2 goal accepted for node %s", node_id.c_str());
  };
  options.result_callback = [this, token, step, node_id](const Nav2GoalHandle::WrappedResult& result) 
  {
    if (token != step_token_ || step != active_step_) {
      RCLCPP_DEBUG(get_logger(), "Ignoring stale Nav2 result for node %s", node_id.c_str());
      return;
    }
    current_goal_handle_.reset();
    set_driving(false);
    if (result.code == rclcpp_action::ResultCode::SUCCEEDED) 
    {
      finish_step(step, NavigateToNode::Result::REACHED, "node reached");
      return;
    }
    // Aborted, cancelled by another Nav2 client or unknown: retry until the dispatch timeout.
    RCLCPP_WARN(get_logger(), "Navigation to %s ended with code=%d — will retry", node_id.c_str(),static_cast<int>(result.code));
    arm_nav2_retry();
  };
  nav2_client_->async_send_goal(goal, options);
}

// Cancels the current Nav2 goal, if any, and drops its pending callbacks.
void BridgeNode::cancel_navigation()
{
  ++step_token_;
  if (!current_goal_handle_) 
  {
    return;
  }
  auto goal_handle = current_goal_handle_;
  current_goal_handle_.reset();
  nav2_client_->async_cancel_goal(goal_handle);
  RCLCPP_INFO(get_logger(), "Navigation cancel requested");
}

// Retries run_step() every nav2_retry_period_sec until the step's timeout.
void BridgeNode::arm_nav2_retry()
{
  if (nav2_retry_timer_) {
    return;
  }
  if (!nav2_retry_deadline_set_) {
    nav2_retry_deadline_ = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(nav2_dispatch_timeout_sec_));
    nav2_retry_deadline_set_ = true;
  }

  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(nav2_retry_period_sec_));
  nav2_retry_timer_ = create_wall_timer(period, [this, period]() 
  {
    nav2_retry_timer_->cancel();
    nav2_retry_timer_.reset();
    if (!active_step_) {
      return;
    }
    if (last_operating_mode_ == "MANUAL") 
    {
      // A human has taken over -- don't burn the retry budget while they're driving.
      nav2_retry_deadline_ += period;
      arm_nav2_retry();
      return;
    }
    if (std::chrono::steady_clock::now() >= nav2_retry_deadline_) 
    {
      RCLCPP_ERROR(get_logger(), "Node %s not reached within %.0f s -- failing the step",active_step_->get_goal()->node.node_id.c_str(), nav2_dispatch_timeout_sec_);
      finish_step(active_step_, NavigateToNode::Result::FAILED,
                  "Dispatch did not become possible or kept failing within the retry window " 
                  "(Nav2 unavailable, robot pose not valid, or repeated navigation failures)");
      set_driving(false);
      return;
    }
    ++step_token_;
    run_step();
  });
}

// Stops the retry timer; the next retry starts a new window.
void BridgeNode::reset_nav2_retry()
{
  if (nav2_retry_timer_) 
  {
    nav2_retry_timer_->cancel();
    nav2_retry_timer_.reset();
  }
  nav2_retry_deadline_set_ = false;
}

// ─── Status ──────────────────────────────────────────────────────────────────

// Publishes the ActionState of `action`.
void BridgeNode::publish_action_feedback(const vda5050_msgs::msg::Action& action,const std::string& status, const std::string& description)
{
  vda5050_msgs::msg::ActionState state;
  state.action_id = action.action_id;
  state.action_type = action.action_type;
  state.action_description = action.action_description;
  state.action_status = status;
  state.result_description = description;
  action_state_feedback_pub_->publish(state);
}

// Publishes driving on change; a stop re-anchors the stale-pose odometry baseline.
void BridgeNode::set_driving(bool driving)
{
  if (driving) {
    has_driven_since_last_amcl_pose_ = true;
  } else if (driving_ && last_odom_position_valid_) 
  {
    odom_x_at_last_amcl_pose_ = last_odom_x_;
    odom_y_at_last_amcl_pose_ = last_odom_y_;
    odom_at_last_amcl_pose_valid_ = true;
    has_driven_since_last_amcl_pose_ = false;
  }
  if (driving == driving_)
  {
    return;
  }
  driving_ = driving;
  publish_driver_status();
}

// Publishes the session id and driving flag.
void BridgeNode::publish_driver_status()
{
  vda5050_msgs::msg::DriverStatus status;
  status.session_id = session_id_;
  status.driving = driving_;
  driver_status_pub_->publish(status);
}

std::string BridgeNode::adapter_topic(const std::string& leaf) const
{
  if (adapter_ns_.empty()) 
  {
    return "/" + leaf;
  }
  if (adapter_ns_.back() == '/') 
  {
    return adapter_ns_ + leaf;
  }
  return adapter_ns_ + "/" + leaf;
}

}  // namespace tb3_vda5050_bridge
