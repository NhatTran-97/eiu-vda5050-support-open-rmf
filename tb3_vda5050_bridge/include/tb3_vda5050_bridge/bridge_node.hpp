#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

// ROS2 standard messages
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/battery_state.hpp>
#include <std_msgs/msg/bool.hpp>
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
#include "tb3_vda5050_bridge/order_session.hpp"

namespace tb3_vda5050_bridge {

/**
 * @brief Bridge between VDA5050 adapter (ROS2) and TurtleBot3 (Nav2).
 *
 * Converts VDA5050 orders into Nav2 navigation goals and publishes robot telemetry back
 * to the adapter. Manages order traversal, state transitions, and goal lifecycle.
 *
 * Key responsibilities:
 *  - Subscribe to adapter topics (order, actions, cancellations)
 *  - Publish robot state (position, velocity, battery, navigation events)
 *  - Manage Nav2 goal dispatch and result handling with token-based staleness guard
 *  - Track order progress and persist state across restarts
 *  - Handle AMCL pose confidence and odometry-based distance accumulation
 */
class BridgeNode : public rclcpp::Node
{
public:
  /**
   * @brief Initialize bridge node: load config, setup ROS interfaces, create Nav2 action client.
   * @param options ROS2 node options.
   */
  explicit BridgeNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

private:
  using NavigateToPose = nav2_msgs::action::NavigateToPose;
  using GoalHandle     = rclcpp_action::ClientGoalHandle<NavigateToPose>;

  // ── Parameters ─────────────────────────────────────────────────────────────
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
  double      pose_stale_move_tolerance_m_{0.15};  // odometry drift allowed before a stale AMCL pose is distrusted

  // ── Subscribers (TB3 / Adapter → Bridge) ──────────────────────────────────
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr                          odom_sub_;
  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr    amcl_pose_sub_;
  rclcpp::Subscription<sensor_msgs::msg::BatteryState>::SharedPtr                   battery_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Order>::SharedPtr         order_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr            action_cancel_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Action>::SharedPtr        action_execute_sub_;

  // ── Publishers (Bridge → Adapter) ──────────────────────────────────────────
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

  // ── Nav2 action client ──────────────────────────────────────────────────────
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav2_client_;

  rclcpp::TimerBase::SharedPtr nav2_retry_timer_;  // retries the pending Nav2 dispatch until success or timeout
  std::chrono::steady_clock::time_point nav2_retry_deadline_;
  bool                  nav2_retry_deadline_set_{false};  // whether nav2_retry_deadline_ is currently armed
  uint64_t              nav2_retry_generation_{0};
  std::size_t           nav2_retry_node_index_{0};

  // ── State ───────────────────────────────────────────────────────────────────
  // Not mutex-protected: safe only because this node spins single-threaded.
  GoalHandle::SharedPtr current_goal_handle_;
  GoalHandle::SharedPtr goal_pending_preemption_;  // goal a fresh dispatch is meant to replace; cancelled if the replacement is rejected
  BridgeStateMachine    state_machine_;
  OrderSession          order_session_;
  uint64_t              navigation_token_{0};
  std::optional<bool>   last_driving_;  // unset until the first publish_bridge_status() call
  std::optional<bool>   last_paused_;
  double                robot_x_{0.0}, robot_y_{0.0}, robot_yaw_{0.0};
  bool                  robot_pose_confident_{false};  // last AMCL pose had finite coords and covariance under threshold
  std::chrono::steady_clock::time_point last_amcl_pose_at_;
  double                amcl_pose_timeout_sec_{10.0};
  double                odom_x_at_last_amcl_pose_{0.0};  // odom snapshot at the last confident AMCL pose
  double                odom_y_at_last_amcl_pose_{0.0};
  bool                  odom_at_last_amcl_pose_valid_{false};
  bool                  has_driven_since_last_amcl_pose_{false};  // whether the robot has driven since the odom snapshot above
  bool                  goal_sent_this_dispatch_{false};  // set when a goal is handed to Nav2 this dispatch cycle
  double                distance_since_last_node_{0.0};  // distance driven since the last node_reached event
  double                last_odom_x_{0.0}, last_odom_y_{0.0};
  bool                  last_odom_position_valid_{false};
  float                 last_battery_charge_{0.0f};  // last known-good battery reading
  bool                  last_battery_valid_{false};

  // ── Callbacks ───────────────────────────────────────────────────────────────
  // Handle incoming odometry (msg): extract position/velocity, track distance driven.
  void on_odom(const nav_msgs::msg::Odometry::SharedPtr msg);
  // Handle AMCL pose (msg): update cached robot pose, check covariance confidence, publish position to adapter.
  void on_amcl_pose(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);
  // Handle battery state (msg): normalize reading (handle both TB3 0-100 and ROS 0-1 formats), publish to adapter.
  void on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg);
  // Handle new/updated order (msg): start order or merge update, reject if stale, persist progress, dispatch work.
  void on_order(const vda5050_msgs::msg::Order::SharedPtr msg);
  // Parse action_cancel (msg) payload ("pause:", "resume:", "cancel:") and dispatch accordingly.
  void on_action_cancel(const std_msgs::msg::String::SharedPtr msg);
  // Process action (msg): handle initPosition, report success/failure, no-op unsupported types.
  void on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg);
  // Send operator's x/y/theta (from action) to AMCL for initial pose; fails if order active.
  void init_position(const vda5050_msgs::msg::Action& action);

  // Caps Nav2's speed at max_speed (m/s); max_speed < 0 lifts any previous cap.
  void apply_speed_limit(double max_speed);

  // Whether the last AMCL pose is confident and recent enough to trust for navigation.
  bool robot_pose_valid() const;

  // ── Nav2 helpers ────────────────────────────────────────────────────────────
  // Cancel current Nav2 goal if active, invalidate pending results.
  void cancel_navigation();
  // Drive order forward: plan next work, dispatch navigation or handle no-op/release waits, retry on failure.
  void dispatch_next_work();
  // Start retry timer: polls dispatch_next_work() every 2s until timeout exhausted or goal succeeds.
  void arm_nav2_retry();
  // Cancel active retry timer, no-op if not armed.
  void cancel_nav2_retry();
  // Fail active order with reason (reason), publish error, persist as complete.
  void fail_stuck_order(const std::string& reason);
  // Check if robot is already within target tolerances; if so, complete node locally without Nav2.
  bool try_complete_in_place(const NavigationTarget& target);
  // Send Nav2 goal for target (target), or arm retry if Nav2 action server not ready yet.
  void send_navigation_goal(const NavigationTarget& target);
  // Publish edge_entered, edge_completed, node_reached events from traversal (events), reset distance counter.
  void publish_traversal_events(const std::vector<TraversalEvent>& events);
  // Publish ActionState feedback for action (action) with status/description.
  void publish_action_feedback(const vda5050_msgs::msg::Action& action,
                               const std::string& status,
                               const std::string& description = "");
  // Publish VDA5050 navigationError with description and optional node_id reference.
  void publish_navigation_error(const std::string& description,
                                const std::string& node_id = "");
  // Publish current driving/paused state to adapter, re-anchor odometry baseline on transitions.
  void publish_bridge_status();
  // Resolve adapter namespace + leaf topic name; e.g. "/vda5050_client_adapter/order".
  std::string adapter_topic(const std::string& leaf) const;
  // Increment navigation_token_ to discard pending Nav2 results; reset current goal handle.
  void invalidate_navigation_context();
  // Publish driving state (driving) only on change.
  void set_driving(bool driving);
  // Publish paused state (paused) only on change.
  void set_paused(bool paused);

  // ── Order progress persistence ─────────────────────────────────────────────
  // Write order state (order_id, cursor, terminal) to file for crash recovery.
  void persist_order_state(const std::string& order_id, std::size_t cursor, bool terminal);
  // Read order state from file into (order_id, cursor, terminal); return false if absent/broken.
  bool load_order_state(std::string& order_id, std::size_t& cursor, bool& terminal) const;
};

}  // namespace tb3_vda5050_bridge
