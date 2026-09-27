#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

// ROS 2 messages
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>

// Nav2 interfaces
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav2_msgs/msg/speed_limit.hpp>

// VDA5050 interfaces
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

// Executes VDA5050 navigation steps through Nav2.  Publishes TurtleBot3 state and action feedback to the client adapter.
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
  std::string map_id_;          // VDA5050 map identifier.
  std::string nav2_frame_id_;   // TF frame for Nav2 goals.
  double      position_covariance_threshold_;
  std::string adapter_ns_;
  std::string odom_topic_;
  std::string amcl_pose_topic_;
  std::string initial_pose_topic_;
  std::string battery_topic_;
  std::string nav2_action_name_;
  std::vector<std::string> supported_action_types_;  // Supported VDA5050 action types.
  bool        simulate_charging_{false};  // Enables simulated startCharging and stopCharging actions.
  double      nav2_dispatch_timeout_sec_;  // Maximum navigation dispatch time.
  std::string speed_limit_topic_;  // Nav2 speed-limit input topic.
  double      pose_stale_move_tolerance_m_{0.15};
  double      nav2_retry_period_sec_{2.0};          // Navigation retry period.
  double      default_allowed_deviation_xy_{0.5};  // Default node-position tolerance in meters.
  double      unconstrained_theta_rad_{3.0};       // Heading-tolerance threshold for unconstrained goals.
  double      battery_voltage_full_{12.6};         // Full-battery voltage fallback.
  double      battery_voltage_empty_{9.0};
  double      initial_pose_covariance_xy_{0.25};   // Initial-pose covariance.
  double      initial_pose_covariance_yaw_{0.06853891945200942};
  double      odom_publish_min_interval_sec_{0.1};  // Velocity and distance publication interval.
  double      driver_status_lease_sec_{1.0};        // Driver-status liveliness lease.
  std::string diagnostics_topic_;
  std::string twist_mux_status_name_;               // twist_mux diagnostic name.
  std::string navigation_velocity_source_;          // Nav2 velocity-source name.

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

  // Active step and Nav2 goal for the single-threaded executor.
  std::shared_ptr<StepHandle> active_step_;
  std::string           last_step_order_id_;
  uint64_t              step_token_{0};         // Invalidates stale Nav2 callbacks.
  bool                  goal_sent_{false};      // Nav2 goal state for the active step.
  Nav2GoalHandle::SharedPtr current_goal_handle_;
  rclcpp::TimerBase::SharedPtr cancel_check_timer_;
  rclcpp::TimerBase::SharedPtr driver_status_timer_;
  rclcpp::TimerBase::SharedPtr nav2_retry_timer_;
  std::chrono::steady_clock::time_point nav2_retry_deadline_;
  bool                  nav2_retry_deadline_set_{false};

  std::string           session_id_;
  bool                  driving_{false};
  double                robot_x_{0.0}, robot_y_{0.0}, robot_yaw_{0.0};
  bool                  robot_pose_confident_{false};  // Validity of the latest AMCL pose.
  std::chrono::steady_clock::time_point last_amcl_pose_at_;
  double                amcl_pose_timeout_sec_{10.0};
  double                odom_x_at_last_amcl_pose_{0.0};  // Odometry snapshot for the latest valid AMCL pose.
  double                odom_y_at_last_amcl_pose_{0.0};
  bool                  odom_at_last_amcl_pose_valid_{false};
  bool                  has_driven_since_last_amcl_pose_{false};  // Motion state since the odometry snapshot.
  OdomDistanceTracker   odom_distance_tracker_;  // VDA5050 distanceSinceLastNode tracker.
  double                last_odom_x_{0.0}, last_odom_y_{0.0};  // Odometry position for the stale-pose check.
  bool                  last_odom_position_valid_{false};
  float                 last_battery_charge_{0.0f};  // Latest valid battery reading.
  bool                  last_battery_valid_{false};
  bool                  hardware_charging_{false};   // Charging state reported by the battery topic.
  bool                  simulated_charging_{false};  // Simulated charger state.
  std::optional<vda5050_msgs::msg::BatteryState> last_battery_state_;  // Last published battery state.
  std::chrono::steady_clock::time_point last_odom_publish_{};

  // Publishes velocity and accumulates traveled distance.
  void on_odom(const nav_msgs::msg::Odometry::SharedPtr msg);
  // Validates the AMCL pose and publishes the robot position.
  void on_amcl_pose(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);
  // Converts the battery reading to VDA5050 battery state.
  void on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg);
  // Publishes MANUAL mode for a non-navigation twist_mux source.
  void on_diagnostics(const diagnostic_msgs::msg::DiagnosticArray::SharedPtr msg);
  // Executes an instant action and publishes its result.
  void on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg);
  // Publishes an AMCL initial pose when navigation is inactive.
  void init_position(const vda5050_msgs::msg::Action& action);
  // Processes startCharging and stopCharging actions.
  void set_charging(const vda5050_msgs::msg::Action& action);
  // Publishes the cached battery state with the current charging flag.
  void publish_battery_state();
  // Drops the active step for a local cancel command.
  void on_local_command(const std_msgs::msg::String::SharedPtr msg);

  // NavigateToNode server callbacks.
  rclcpp_action::GoalResponse on_step_goal(const rclcpp_action::GoalUUID& uuid,
                                           std::shared_ptr<const NavigateToNode::Goal> goal);
  rclcpp_action::CancelResponse on_step_cancel(const std::shared_ptr<StepHandle> handle);
  void on_step_accepted(const std::shared_ptr<StepHandle> handle);
  // Completes a requested step cancellation.
  void check_step_cancel();

  // Completes, dispatches, or retries the active step.
  void run_step();
  // Completes a step and clears it when active.
  void finish_step(std::shared_ptr<StepHandle> handle, uint8_t outcome, const std::string& description);

  // Applies a Nav2 speed limit; a negative value removes the limit.
  void apply_speed_limit(double max_speed);
  // Returns true when the AMCL pose is valid for navigation.
  bool robot_pose_valid() const;
  // Returns true when the robot satisfies the node tolerances.
  bool at_node(const vda5050_msgs::msg::Node& node) const;
  // Sends the active Nav2 goal or schedules a retry.
  void send_navigation_goal();
  // Cancels the active Nav2 goal and invalidates pending callbacks.
  void cancel_navigation();
  // Retries the active step until completion or timeout.
  void arm_nav2_retry();
  // Stops the retry timer and clears its deadline.
  void reset_nav2_retry();
  // Publishes the action state.
  void publish_action_feedback(const vda5050_msgs::msg::Action& action,
                               const std::string& status,
                               const std::string& description = "");
  // Publishes driving-state changes and resets the odometry baseline.
  void set_driving(bool driving);
  // Publishes driver status and asserts topic liveliness.
  void publish_driver_status();
  // Returns the fully qualified client-adapter topic.
  std::string adapter_topic(const std::string& leaf) const;
};

}  // namespace tb3_vda5050_bridge
