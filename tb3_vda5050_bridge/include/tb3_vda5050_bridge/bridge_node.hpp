#pragma once

#include <cstdint>
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

class BridgeNode : public rclcpp::Node
{
public:
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
  std::string battery_topic_;
  std::string nav2_action_name_;
  std::vector<std::string> supported_action_types_;  // VDA5050 action types this bridge implements

  // ── Subscribers (TB3 / Adapter → Bridge) ──────────────────────────────────
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr                          odom_sub_;
  rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr    amcl_pose_sub_;
  rclcpp::Subscription<sensor_msgs::msg::BatteryState>::SharedPtr                   battery_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Order>::SharedPtr         order_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr            action_cancel_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Action>::SharedPtr        action_execute_sub_;

  // ── Publishers (Bridge → Adapter) ──────────────────────────────────────────
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

  // ── State ───────────────────────────────────────────────────────────────────
  GoalHandle::SharedPtr current_goal_handle_;
  BridgeStateMachine    state_machine_;
  OrderSession          order_session_;
  uint64_t              navigation_token_{0};
  bool                  last_driving_{false};
  bool                  last_paused_{false};
  double                robot_x_{0.0}, robot_y_{0.0}, robot_yaw_{0.0};
  bool                  robot_pose_valid_{false};

  // ── Callbacks ───────────────────────────────────────────────────────────────
  void on_odom(const nav_msgs::msg::Odometry::SharedPtr msg);
  void on_amcl_pose(const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg);
  void on_battery(const sensor_msgs::msg::BatteryState::SharedPtr msg);
  void on_order(const vda5050_msgs::msg::Order::SharedPtr msg);
  void on_action_cancel(const std_msgs::msg::String::SharedPtr msg);
  void on_action_execute(const vda5050_msgs::msg::Action::SharedPtr msg);

  // ── Nav2 helpers ────────────────────────────────────────────────────────────
  void cancel_navigation();
  void dispatch_next_work();
  bool try_complete_in_place(const NavigationTarget& target);
  void send_navigation_goal(const NavigationTarget& target);
  void publish_traversal_events(const std::vector<TraversalEvent>& events);
  void publish_action_feedback(const vda5050_msgs::msg::Action& action,
                               const std::string& status,
                               const std::string& description = "");
  void publish_navigation_error(const std::string& description,
                                const std::string& node_id = "");
  void publish_bridge_status();
  std::string adapter_topic(const std::string& leaf) const;
  void invalidate_navigation_context();
  void set_driving(bool driving);
  void set_paused(bool paused);
};

}  // namespace tb3_vda5050_bridge
