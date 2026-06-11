#pragma once

/**
 * @file vda5050_node.hpp
 * @brief VDA5050 v2.1.0 adapter — ROS2 node interface.
 *
 * `VDA5050Node` is the ROS/MQTT wiring layer.
 * High-level runtime transitions are delegated to `AdapterStateMachine`,
 * while `OrderManager` and `ActionManager` remain responsible for their
 * own domain lifecycles.
 *
 * ROS2 I/O Overview:
 *
 *  Published (adapter → robot):
 *    ~/order              vda5050_msgs/Order
 *    ~/action_execute     vda5050_msgs/Action
 *    ~/action_cancel      std_msgs/String    ("pause:id"|"resume:id"|"cancel:id")
 *
 *  Subscribed (robot → adapter):
 *    ~/agv_position       vda5050_msgs/AgvPosition
 *    ~/velocity           vda5050_msgs/Velocity
 *    ~/battery_state      vda5050_msgs/BatteryState
 *    ~/driving            std_msgs/Bool
 *    ~/paused             std_msgs/Bool
 *    ~/action_state_feedback
 *                         vda5050_msgs/ActionState  (robot reports status changes)
 *    ~/error              vda5050_msgs/Error
 *    ~/safety_state       vda5050_msgs/SafetyState
 *    ~/operating_mode     std_msgs/String
 *    ~/load               vda5050_msgs/Load
 *    ~/node_reached       vda5050_msgs/NodeState
 *    ~/edge_entered       vda5050_msgs/EdgeState
 *    ~/edge_completed     vda5050_msgs/EdgeState
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>

#include <vda5050_msgs/msg/order.hpp>
#include <vda5050_msgs/msg/state.hpp>
#include <vda5050_msgs/msg/connection.hpp>
#include <vda5050_msgs/msg/visualization.hpp>
#include <vda5050_msgs/msg/action.hpp>
#include <vda5050_msgs/msg/action_state.hpp>
#include <vda5050_msgs/msg/agv_position.hpp>
#include <vda5050_msgs/msg/velocity.hpp>
#include <vda5050_msgs/msg/battery_state.hpp>
#include <vda5050_msgs/msg/load.hpp>
#include <vda5050_msgs/msg/error.hpp>
#include <vda5050_msgs/msg/safety_state.hpp>
#include <vda5050_msgs/msg/node_state.hpp>
#include <vda5050_msgs/msg/edge_state.hpp>

#include "vda5050_client_adapter/vda5050_types.hpp"
#include "vda5050_client_adapter/adapter_state_machine.hpp"
#include "vda5050_client_adapter/mqtt_client.hpp"
#include "vda5050_client_adapter/order_manager.hpp"
#include "vda5050_client_adapter/action_manager.hpp"

namespace vda5050_adapter {

class VDA5050Node : public rclcpp::Node {
public:
  explicit VDA5050Node(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());
  ~VDA5050Node() override;

private:
  // ── Initialization ─────────────────────────────────────────────────────────
  void declare_and_load_parameters();
  void setup_mqtt();
  void setup_ros_interfaces();
  void teardown_mqtt();
  vda5050::Factsheet build_factsheet_from_params() const;

  // ── MQTT topic factory ─────────────────────────────────────────────────────
  std::string make_topic(const std::string& suffix) const;

  // ── MQTT → ROS2  (Master Control → robot) ─────────────────────────────────
  void on_order_message(const MqttMessage& msg);
  void on_instant_actions_message(const MqttMessage& msg);
  bool handle_instant_action(const vda5050::Action& action);

  // ── ROS2 / timer → MQTT  (robot state → Master Control) ───────────────────
  void publish_state();
  void publish_connection(vda5050::ConnectionState state);
  void publish_visualization();
  void publish_factsheet();

  // ── ROS2 subscribers  (robot → adapter) ────────────────────────────────────
  void on_agv_position(const vda5050_msgs::msg::AgvPosition::SharedPtr msg);
  void on_velocity(const vda5050_msgs::msg::Velocity::SharedPtr msg);
  void on_battery_state(const vda5050_msgs::msg::BatteryState::SharedPtr msg);
  void on_driving(const std_msgs::msg::Bool::SharedPtr msg);
  void on_paused(const std_msgs::msg::Bool::SharedPtr msg);
  void on_action_state_feedback(const vda5050_msgs::msg::ActionState::SharedPtr msg);
  void on_errors(const vda5050_msgs::msg::Error::SharedPtr msg);
  void on_safety_state(const vda5050_msgs::msg::SafetyState::SharedPtr msg);
  void on_operating_mode(const std_msgs::msg::String::SharedPtr msg);
  void on_load(const vda5050_msgs::msg::Load::SharedPtr msg);

  // ── Navigation feedback  (robot reports progress) ─────────────────────────
  void on_node_reached(const vda5050_msgs::msg::NodeState::SharedPtr msg);
  void on_edge_entered(const vda5050_msgs::msg::EdgeState::SharedPtr msg);
  void on_edge_completed(const vda5050_msgs::msg::EdgeState::SharedPtr msg);

  // ── OrderManager callbacks ─────────────────────────────────────────────────
  void on_order_accepted(const std::string& order_id,
                         uint32_t order_update_id,
                         const std::vector<vda5050::Node>& remaining_nodes,
                         const std::vector<vda5050::Edge>& remaining_edges);
  void on_order_cancelled(const std::string& order_id);

  // ── ActionManager callbacks ────────────────────────────────────────────────
  void on_action_execute(const vda5050::Action& action);
  void on_action_pause(const std::string& action_id);
  void on_action_resume(const std::string& action_id);
  void on_action_cancel(const std::string& action_id);
  bool maybe_complete_pending_control_actions();
  void replace_adapter_error(const vda5050::Error& error);
  void upsert_driver_error(const vda5050::Error& error);
  void clear_errors_by_type(const std::string& error_type);
  void sync_order_activity();
  void sync_action_blocking();

  // ── State assembly ─────────────────────────────────────────────────────────
  vda5050::State  build_state_snapshot() const;
  vda5050::Header make_header(const std::string& topic) const;

  // ── Utility ────────────────────────────────────────────────────────────────
  static std::string now_iso8601();

  // ── ROS2 parameters ────────────────────────────────────────────────────────
  std::string broker_url_;
  std::string client_id_;
  std::string username_;
  std::string password_;
  std::string interface_name_;
  std::string manufacturer_;
  std::string serial_number_;
  double      state_publish_interval_{30.0};
  double      visualization_interval_{1.0};

  // ── Core components ────────────────────────────────────────────────────────
  std::unique_ptr<MqttClient>    mqtt_client_;
  std::unique_ptr<AdapterStateMachine> state_machine_;
  std::unique_ptr<OrderManager>  order_manager_;
  std::unique_ptr<ActionManager> action_manager_;
  std::string                    action_state_order_id_;
  vda5050::Factsheet             factsheet_;  // built once from params, published on connect

  // ── Snapshot serialization mutex ──────────────────────────────────────────
  // Guards order_manager_ / action_manager_ access. Must be acquired BEFORE state_mutex_.
  mutable std::mutex            snapshot_mutex_;

  // ── Robot state  (guarded by state_mutex_) ─────────────────────────────────
  mutable std::mutex            state_mutex_;
  vda5050::AgvPosition          agv_position_;
  bool                          agv_position_set_{false};
  vda5050::Velocity             velocity_;
  bool                          velocity_set_{false};
  vda5050::BatteryState         battery_state_;
  vda5050::SafetyState          safety_state_;
  std::vector<vda5050::Load>    loads_;
  std::vector<vda5050::Error>   errors_;
  std::optional<vda5050::NodePosition> last_reached_node_position_;
  bool                          driving_{false};
  bool                          paused_{false};
  vda5050::OperatingMode        operating_mode_{vda5050::OperatingMode::AUTOMATIC};

  // ── Header ID counters per topic  (guarded by hdr_mutex_) ────────────────
  // VDA5050 §7.1: headerId must increment monotonically per message type.
  mutable std::mutex                                     hdr_mutex_;
  mutable std::unordered_map<std::string, uint32_t>     header_ids_;

  // ── ROS2 publishers  (adapter → robot) ────────────────────────────────────
  rclcpp::Publisher<vda5050_msgs::msg::Order>::SharedPtr          order_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::Action>::SharedPtr         action_execute_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr             action_cancel_pub_;

  // ── ROS2 subscribers  (robot → adapter) ───────────────────────────────────
  rclcpp::Subscription<vda5050_msgs::msg::AgvPosition>::SharedPtr  agv_pos_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Velocity>::SharedPtr     velocity_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::BatteryState>::SharedPtr battery_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr             driving_sub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr             paused_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::ActionState>::SharedPtr  action_state_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Error>::SharedPtr        error_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::SafetyState>::SharedPtr  safety_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr           op_mode_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Load>::SharedPtr         load_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::NodeState>::SharedPtr    node_reached_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::EdgeState>::SharedPtr    edge_entered_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::EdgeState>::SharedPtr    edge_completed_sub_;

  // ── Timers ─────────────────────────────────────────────────────────────────
  rclcpp::TimerBase::SharedPtr state_timer_;
  rclcpp::TimerBase::SharedPtr visualization_timer_;
};

}  // namespace vda5050_adapter
