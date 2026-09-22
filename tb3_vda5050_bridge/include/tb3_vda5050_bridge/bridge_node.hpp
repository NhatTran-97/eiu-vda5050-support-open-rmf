#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

// ROS2 standard messages
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>

// Nav2 action
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav2_msgs/msg/speed_limit.hpp>

// VDA5050 messages
#include <vda5050_msgs/msg/order.hpp>
#include <vda5050_msgs/msg/action.hpp>
#include <vda5050_msgs/msg/action_state.hpp>
#include <vda5050_msgs/msg/agv_position.hpp>
#include <vda5050_msgs/msg/battery_state.hpp>
#include <vda5050_msgs/msg/velocity.hpp>
#include <vda5050_msgs/msg/node_state.hpp>
#include <vda5050_msgs/msg/edge_state.hpp>
#include <vda5050_msgs/msg/error.hpp>
#include <vda5050_msgs/msg/error_reference.hpp>

#include "tb3_vda5050_bridge/bridge_state_machine.hpp"
#include "tb3_vda5050_bridge/odom_distance_tracker.hpp"
#include "tb3_vda5050_bridge/order_session.hpp"

namespace tb3_vda5050_bridge {

// Turns VDA5050 orders into Nav2 goals and reports the TB3 state to the adapter.
class BridgeNode : public rclcpp::Node
{
public:
  explicit BridgeNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

private:
  using NavigateToPose = nav2_msgs::action::NavigateToPose;
  using GoalHandle     = rclcpp_action::ClientGoalHandle<NavigateToPose>;

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
  std::string order_state_path_;  // where order progress is persisted across restarts
  double      nav2_dispatch_timeout_sec_;  // max time to wait for Nav2 before failing the order
  std::string speed_limit_topic_;  // Nav2 controller_server's speed override input
  double      pose_stale_move_tolerance_m_{0.15}; 
  // Subscribers
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr                          odom_sub_;
  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr    amcl_pose_sub_;
  rclcpp::Subscription<sensor_msgs::msg::BatteryState>::SharedPtr                   battery_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Order>::SharedPtr         order_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr            action_cancel_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Action>::SharedPtr        action_execute_sub_;
  rclcpp::Subscription<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_sub_;

  // Publishers
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initial_pose_pub_;
  rclcpp::Publisher<nav2_msgs::msg::SpeedLimit>::SharedPtr          speed_limit_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::AgvPosition>::SharedPtr      agv_position_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::BatteryState>::SharedPtr     battery_state_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::Velocity>::SharedPtr         velocity_pub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr                 driving_pub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr                 paused_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::NodeState>::SharedPtr        node_reached_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::EdgeState>::SharedPtr        edge_entered_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::EdgeState>::SharedPtr        edge_completed_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::ActionState>::SharedPtr      action_state_feedback_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::Error>::SharedPtr            error_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr               order_dropped_pub_;
  rclcpp::Publisher<std_msgs::msg::Float64>::SharedPtr              distance_since_last_node_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr               operating_mode_pub_;
  std::string last_operating_mode_{"AUTOMATIC"}; 

  // Nav2 action client
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav2_client_;

  rclcpp::TimerBase::SharedPtr nav2_retry_timer_;  // retries the pending Nav2 dispatch until success or timeout
  std::chrono::steady_clock::time_point nav2_retry_deadline_;
  bool                  nav2_retry_deadline_set_{false};  // whether nav2_retry_deadline_ is currently armed
  uint64_t              nav2_retry_generation_{0};
  std::size_t           nav2_retry_node_index_{0};

  // State; not mutex-protected because the node spins single-threaded.
  GoalHandle::SharedPtr current_goal_handle_;
  GoalHandle::SharedPtr goal_pending_preemption_;  // goal to cancel if its replacement is rejected
  BridgeStateMachine    state_machine_;
  OrderSession          order_session_;
  uint64_t              navigation_token_{0};
  std::optional<bool>   last_driving_;  // unset until the first publish
  std::optional<bool>   last_paused_;
  double                robot_x_{0.0}, robot_y_{0.0}, robot_yaw_{0.0};
  bool                  robot_pose_confident_{false};  // last AMCL pose was finite with covariance under the threshold
  std::chrono::steady_clock::time_point last_amcl_pose_at_;
  double                amcl_pose_timeout_sec_{10.0};
  double                odom_x_at_last_amcl_pose_{0.0};  // odom snapshot at the last confident AMCL pose
  double                odom_y_at_last_amcl_pose_{0.0};
  bool                  odom_at_last_amcl_pose_valid_{false};
  bool                  has_driven_since_last_amcl_pose_{false};  // driven since the odom snapshot
  bool                  goal_sent_this_dispatch_{false};  // a goal went to Nav2 this dispatch cycle
  OdomDistanceTracker   odom_distance_tracker_;  // VDA5050 distanceSinceLastNode telemetry
  double                last_odom_x_{0.0}, last_odom_y_{0.0};  // for the stale-but-stationary pose check
  bool                  last_odom_position_valid_{false};
  float                 last_battery_charge_{0.0f};  // last known-good battery reading
  bool                  last_battery_valid_{false};

  // Publishes velocity and accumulates the distance driven.
  void on_odom(const nav_msgs::msg::Odometry::SharedPtr msg);
  // Caches the AMCL pose, checks its covariance and publishes the position.
  void on_amcl_pose(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);
  // Publishes the battery charge, accepting 0-100 and 0-1 readings.
  void on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg);
  // Detects a manual override from twist_mux diagnostics and publishes operating_mode on change.
  void on_diagnostics(const diagnostic_msgs::msg::DiagnosticArray::SharedPtr msg);
  // Starts an order or merges an update, then dispatches.
  void on_order(const vda5050_msgs::msg::Order::SharedPtr msg);
  // Handles the pause, resume and cancel commands.
  void on_action_cancel(const std_msgs::msg::String::SharedPtr msg);
  // Runs an instant action and reports its result.
  void on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg);
  // Sets the AMCL initial pose; refused while a goal drives from a valid pose.
  void init_position(const vda5050_msgs::msg::Action& action);

  // Caps Nav2's speed at max_speed (m/s); max_speed < 0 lifts any previous cap.
  void apply_speed_limit(double max_speed);

  // Whether the last AMCL pose is confident and recent enough to navigate on.
  bool robot_pose_valid() const;

  // Cancels the current Nav2 goal and ignores its pending result.
  void cancel_navigation();
  // Advances the order: navigates, completes nodes locally or waits for release.
  void dispatch_next_work();
  // Retries dispatch every 2 s until a goal is sent or the timeout expires.
  void arm_nav2_retry();
  // Stops the dispatch retry timer.
  void cancel_nav2_retry();
  // Fails the active order with `reason` and persists it as finished.
  void fail_stuck_order(const std::string& reason);
  // Tells the adapter the order was dropped outside cancelOrder.
  void notify_order_dropped(const std::string& order_id);
  // Completes the node without Nav2 when the robot is already within tolerance.
  bool try_complete_in_place(const NavigationTarget& target);
  // Sends the Nav2 goal for `target`, or arms a retry if Nav2 is not ready.
  void send_navigation_goal(const NavigationTarget& target);
  // Publishes the traversal events and resets the distance counter.
  void publish_traversal_events(const std::vector<TraversalEvent>& events);
  // Publishes the ActionState of `action`.
  void publish_action_feedback(const vda5050_msgs::msg::Action& action,
                               const std::string& status,
                               const std::string& description = "");
  // Publishes a VDA5050 navigationError.
  void publish_navigation_error(const std::string& description,
                                const std::string& node_id = "");
  // Publishes driving and paused, re-anchoring the odometry baseline on transitions.
  void publish_bridge_status();
  // Full adapter topic name, e.g. "/vda5050_client_adapter/order".
  std::string adapter_topic(const std::string& leaf) const;
  // Discards pending Nav2 results and the current goal handle.
  void invalidate_navigation_context();
  // Publishes driving on change only.
  void set_driving(bool driving);
  // Publishes paused on change only.
  void set_paused(bool paused);

  // Saves order progress for recovery after a restart.
  void persist_order_state(const std::string& order_id, std::size_t cursor, bool terminal);
  // Loads saved order progress; false if the file is absent or unreadable.
  bool load_order_state(std::string& order_id, std::size_t& cursor, bool& terminal) const;
};

}  // namespace tb3_vda5050_bridge
