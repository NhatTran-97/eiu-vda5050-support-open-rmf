#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

// ROS2 standard messages
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>

// Nav2 action
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav2_msgs/msg/speed_limit.hpp>

// VDA5050 messages
#include <vda5050_msgs/action/navigate_to_node.hpp>
#include <vda5050_msgs/msg/action.hpp>
#include <vda5050_msgs/msg/action_command.hpp>
#include <vda5050_msgs/msg/action_state.hpp>
#include <vda5050_msgs/msg/agv_position.hpp>
#include <vda5050_msgs/msg/battery_state.hpp>
#include <vda5050_msgs/msg/driver_status.hpp>
#include <vda5050_msgs/msg/velocity.hpp>

#include "tb3_vda5050_bridge/odom_distance_tracker.hpp"

namespace tb3_vda5050_bridge {

// Executes NavigateToNode goals of the client adapter with Nav2 and reports the TB3 state.
// The client adapter owns the order; this node drives one node at a time.
class BridgeNode : public rclcpp::Node
{
public:
  explicit BridgeNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

private:
  using NavigateToPose = nav2_msgs::action::NavigateToPose;
  using Nav2GoalHandle = rclcpp_action::ClientGoalHandle<NavigateToPose>;
  using NavigateToNode = vda5050_msgs::action::NavigateToNode;
  using StepHandle     = rclcpp_action::ServerGoalHandle<NavigateToNode>;

  // Parameters
  std::string map_id_;          // VDA5050 logical map name (reported in agv_position)
  std::string nav2_frame_id_;   // TF frame for Nav2 goals (global_costmap.global_frame)
  double      position_covariance_threshold_;
  std::string adapter_ns_;
  std::string odom_topic_;
  std::string amcl_pose_topic_;
  std::string initial_pose_topic_;
  std::string battery_topic_;
  std::string nav2_action_name_;
  std::vector<std::string> supported_action_types_;  // VDA5050 action types this bridge implements
  double      nav2_dispatch_timeout_sec_;  // max time to reach a node before its goal fails
  std::string speed_limit_topic_;  // Nav2 controller_server's speed override input
  double      pose_stale_move_tolerance_m_{0.15};
  double      nav2_retry_period_sec_{2.0};          // period of the dispatch retry timer
  double      default_allowed_deviation_xy_{0.5};  // node tolerance when the order gives none (m)
  double      unconstrained_theta_rad_{3.0};       // allowedDeviationTheta at or above this ignores heading
  double      battery_voltage_full_{12.6};         // voltage fallback for the charge (V)
  double      battery_voltage_empty_{9.0};
  double      initial_pose_covariance_xy_{0.25};   // initPosition covariance (m^2, rad^2)
  double      initial_pose_covariance_yaw_{0.06853891945200942};
  double      odom_publish_min_interval_sec_{0.1};  // velocity / distance telemetry throttle
  double      driver_status_lease_sec_{1.0};        // driver_status liveliness lease; republished every lease / 3
  std::string diagnostics_topic_;
  std::string twist_mux_status_name_;               // twist_mux diagnostic status
  std::string navigation_velocity_source_;          // twist_mux input used by Nav2

  // Subscribers
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr                       odom_sub_;
  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr amcl_pose_sub_;
  rclcpp::Subscription<sensor_msgs::msg::BatteryState>::SharedPtr                battery_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Action>::SharedPtr                     action_execute_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::ActionCommand>::SharedPtr              action_command_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr                         local_command_sub_;
  rclcpp::Subscription<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr         diagnostics_sub_;

  // Publishers
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initial_pose_pub_;
  rclcpp::Publisher<nav2_msgs::msg::SpeedLimit>::SharedPtr          speed_limit_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::AgvPosition>::SharedPtr      agv_position_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::BatteryState>::SharedPtr     battery_state_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::Velocity>::SharedPtr         velocity_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::DriverStatus>::SharedPtr     driver_status_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::ActionState>::SharedPtr      action_state_feedback_pub_;
  rclcpp::Publisher<std_msgs::msg::Float64>::SharedPtr              distance_since_last_node_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr               operating_mode_pub_;
  std::string last_operating_mode_{"AUTOMATIC"};

  // Step server (client adapter) and Nav2 client
  rclcpp_action::Server<NavigateToNode>::SharedPtr step_server_;
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav2_client_;

  // Active step and its Nav2 goal; single-threaded executor, no locking.
  std::shared_ptr<StepHandle> active_step_;
  std::string           last_step_order_id_;
  uint64_t              step_token_{0};         // bumped per step; drops stale Nav2 callbacks
  bool                  goal_sent_{false};      // a Nav2 goal went out for the current step
  Nav2GoalHandle::SharedPtr current_goal_handle_;
  rclcpp::TimerBase::SharedPtr cancel_check_timer_;
  rclcpp::TimerBase::SharedPtr driver_status_timer_;
  rclcpp::TimerBase::SharedPtr nav2_retry_timer_;
  std::chrono::steady_clock::time_point nav2_retry_deadline_;
  bool                  nav2_retry_deadline_set_{false};

  std::string           session_id_;
  bool                  driving_{false};
  double                robot_x_{0.0}, robot_y_{0.0}, robot_yaw_{0.0};
  bool                  robot_pose_confident_{false};  // last AMCL pose was finite with covariance under the threshold
  std::chrono::steady_clock::time_point last_amcl_pose_at_;
  double                amcl_pose_timeout_sec_{10.0};
  double                odom_x_at_last_amcl_pose_{0.0};  // odom snapshot at the last confident AMCL pose
  double                odom_y_at_last_amcl_pose_{0.0};
  bool                  odom_at_last_amcl_pose_valid_{false};
  bool                  has_driven_since_last_amcl_pose_{false};  // driven since the odom snapshot
  OdomDistanceTracker   odom_distance_tracker_;  // VDA5050 distanceSinceLastNode telemetry
  double                last_odom_x_{0.0}, last_odom_y_{0.0};  // for the stale-but-stationary pose check
  bool                  last_odom_position_valid_{false};
  float                 last_battery_charge_{0.0f};  // last known-good battery reading
  bool                  last_battery_valid_{false};
  std::chrono::steady_clock::time_point last_odom_publish_{};

  // Publishes velocity and accumulates the distance driven.
  void on_odom(const nav_msgs::msg::Odometry::SharedPtr msg);
  // Caches the AMCL pose, checks its covariance and publishes the position.
  void on_amcl_pose(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);
  // Publishes the battery charge, accepting 0-100 and 0-1 readings.
  void on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg);
  // Detects a manual override from twist_mux diagnostics and publishes operating_mode on change.
  void on_diagnostics(const diagnostic_msgs::msg::DiagnosticArray::SharedPtr msg);
  // Runs an instant action and reports its result.
  void on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg);
  // Sets the AMCL initial pose; refused while a goal drives from a valid pose.
  void init_position(const vda5050_msgs::msg::Action& action);
  // Handles "cancel:*" from local tools: drops the active step.
  void on_local_command(const std_msgs::msg::String::SharedPtr msg);

  // NavigateToNode server callbacks.
  rclcpp_action::GoalResponse on_step_goal(const rclcpp_action::GoalUUID& uuid,
                                           std::shared_ptr<const NavigateToNode::Goal> goal);
  rclcpp_action::CancelResponse on_step_cancel(const std::shared_ptr<StepHandle> handle);
  void on_step_accepted(const std::shared_ptr<StepHandle> handle);
  // Finishes steps whose cancellation was requested.
  void check_step_cancel();

  // Works on the active step: completes it in place, sends the Nav2 goal or arms a retry.
  void run_step();
  // Finishes `handle` with `outcome`; clears the active step when it is `handle`.
  void finish_step(std::shared_ptr<StepHandle> handle, uint8_t outcome, const std::string& description);

  // Caps Nav2's speed at max_speed (m/s); max_speed < 0 lifts any previous cap.
  void apply_speed_limit(double max_speed);
  // Whether the last AMCL pose is confident and recent enough to navigate on.
  bool robot_pose_valid() const;
  // Whether the robot already stands on `node` within its tolerances.
  bool at_node(const vda5050_msgs::msg::Node& node) const;
  // Sends the Nav2 goal for the active step, or arms a retry if Nav2 is not ready.
  void send_navigation_goal();
  // Cancels the current Nav2 goal and ignores its pending result.
  void cancel_navigation();
  // Retries run_step() every nav2_retry_period_sec until the step reaches its node or the timeout expires.
  void arm_nav2_retry();
  // Stops the retry timer and forgets the retry window.
  void reset_nav2_retry();
  // Publishes the ActionState of `action`.
  void publish_action_feedback(const vda5050_msgs::msg::Action& action,
                               const std::string& status,
                               const std::string& description = "");
  // Publishes driving on change, re-anchoring the odometry baseline on transitions.
  void set_driving(bool driving);
  // Publishes session id and driving on ~/driver_status; each publish asserts its liveliness.
  void publish_driver_status();
  // Full adapter topic name, e.g. "/vda5050_client_adapter/navigate_to_node".
  std::string adapter_topic(const std::string& leaf) const;
};

}  // namespace tb3_vda5050_bridge
