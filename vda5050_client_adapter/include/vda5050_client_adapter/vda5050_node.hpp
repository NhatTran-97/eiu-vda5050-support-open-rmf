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
 *  Adapter → robot:
 *    ~/navigate_to_node   vda5050_msgs/action/NavigateToNode (client: one goal per route node)
 *    ~/action_execute     vda5050_msgs/Action
 *    ~/action_command     vda5050_msgs/ActionCommand (pause / resume / cancel of one action)
 *
 *  Local status for on-robot tools (robot_local_ui):
 *    ~/driving, ~/paused  std_msgs/Bool (latched)
 *    ~/node_reached       vda5050_msgs/NodeState
 *    ~/error              vda5050_msgs/Error (navigationError of a failed step)
 *
 *  Robot → adapter:
 *    ~/driver_status      vda5050_msgs/DriverStatus (latched: session id + driving; liveliness = driver alive)
 *    ~/agv_position       vda5050_msgs/AgvPosition
 *    ~/velocity           vda5050_msgs/Velocity
 *    ~/battery_state      vda5050_msgs/BatteryState
 *    ~/action_state_feedback
 *                         vda5050_msgs/ActionState
 *    ~/error              vda5050_msgs/Error
 *    ~/safety_state       vda5050_msgs/SafetyState
 *    ~/operating_mode     std_msgs/String    (latched)
 *    ~/load               vda5050_msgs/Load
 *    ~/distance_since_last_node
 *                         std_msgs/Float64   (live progress on the current leg)
 */

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/string.hpp>

#include <vda5050_msgs/action/navigate_to_node.hpp>
#include <vda5050_msgs/msg/action_command.hpp>
#include <vda5050_msgs/msg/driver_status.hpp>
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

#include "vda5050_client_adapter/vda5050_types.hpp"
#include "vda5050_client_adapter/adapter_state_machine.hpp"
#include "vda5050_client_adapter/mqtt_client.hpp"
#include "vda5050_client_adapter/order_manager.hpp"
#include "vda5050_client_adapter/action_manager.hpp"

namespace vda5050_adapter {

/**
 * @brief VDA5050 adapter node: ROS2/MQTT bridge for autonomous mobile robots.
 *
 * Mediates between Master Control (MQTT/VDA5050 JSON) and the robot driver (ROS2).
 * Owns the order: drives the route one node at a time through NavigateToNode goals and
 * applies their results to the order, action and state tracking.
 * Threading: every handler runs on the executor thread. MQTT callbacks (Paho thread) only
 * queue work through post(); event_timer_ runs it and publishes the state at most once per
 * tick, so the node needs a single-threaded executor and no locks of its own.
 *
 * Key responsibilities:
 *  - Execute the route on the driver, gated by pause and (strict mode) blocking actions
 *  - Subscribe to robot telemetry (position, velocity, battery, driver status, action feedback)
 *  - Publish VDA5050 state, visualization, connection, and factsheet to MQTT
 *  - Handle inbound orders and instantActions from Master Control
 *  - Manage order state and route traversal via OrderManager
 *  - Execute and track actions (NONE/SOFT/HARD blocking) via ActionManager
 *  - Coordinate adapter-wide mode transitions via AdapterStateMachine
 */
class VDA5050Node : public rclcpp::Node {
public:
  /**
   * @brief Initialize adapter node: load config, setup MQTT and ROS2 interfaces.
   * @param options ROS2 node options.
   */
  explicit VDA5050Node(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());

  /**
   * @brief Graceful shutdown: disconnect MQTT and clean up resources.
   */
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

  // ── Executor hand-off ─────────────────────────────────────────────────────
  // Queue work from another thread for the executor thread.
  void post(std::function<void()> work);
  // Run queued work, then publish the state if it changed (event_timer_).
  void on_event_tick();

  // ── MQTT → ROS2  (Master Control → robot) ─────────────────────────────────
  void on_mqtt_connection_changed(bool connected);
  void on_order_message(const MqttMessage& msg);
  void on_instant_actions_message(const MqttMessage& msg);
  bool handle_instant_action(const vda5050::Action& action);

  // ── ROS2 / timer → MQTT  (robot state → Master Control) ───────────────────
  // Mark the state as changed; it is published on the next event tick.
  void publish_state();
  // Publish the state now.
  void flush_state();
  void publish_connection(vda5050::ConnectionState state);
  void publish_visualization();
  void publish_factsheet();

  // ── ROS2 subscribers  (robot → adapter) ────────────────────────────────────
  void on_agv_position(const vda5050_msgs::msg::AgvPosition::SharedPtr msg);
  void on_velocity(const vda5050_msgs::msg::Velocity::SharedPtr msg);
  void on_battery_state(const vda5050_msgs::msg::BatteryState::SharedPtr msg);
  // Driving flag; a new session id means the driver restarted and lost the step in flight.
  void on_driver_status(const vda5050_msgs::msg::DriverStatus::SharedPtr msg);
  // driver_status liveliness: lost drops the step in flight and raises driverConnectionError (FATAL); back clears it.
  void on_driver_liveliness(const rclcpp::QOSLivelinessChangedInfo& info);
  // Driver driving flag to the state machine and ~/driving; true if it changed.
  bool set_driver_driving(bool driving);
  void on_action_state_feedback(const vda5050_msgs::msg::ActionState::SharedPtr msg);
  void on_errors(const vda5050_msgs::msg::Error::SharedPtr msg);
  void on_safety_state(const vda5050_msgs::msg::SafetyState::SharedPtr msg);
  void on_operating_mode(const std_msgs::msg::String::SharedPtr msg);
  void on_load(const vda5050_msgs::msg::Load::SharedPtr msg);

  // Live distance-since-last-node reading, streamed continuously by the driver.
  void on_distance_since_last_node(const std_msgs::msg::Float64::SharedPtr msg);

  // ── Route execution  (NavigateToNode goals on the driver) ─────────────────
  using NavigateToNode = vda5050_msgs::action::NavigateToNode;
  using StepGoalHandle = rclcpp_action::ClientGoalHandle<NavigateToNode>;

  // Step goal in flight; results of any other goal are ignored.
  struct InFlightStep {
    uint64_t                  token{0};
    RouteStep                 step;
    StepGoalHandle::SharedPtr handle;             // set once the driver accepts the goal
    bool                      cancel_requested{false};
    std::string               session;            // driver session the goal was sent to
  };

  // Sends, replaces or cancels the step goal so that the driver heads for the next node while allowed.
  void drive();
  // Route may advance: no pause requested and, in strict mode, no HARD/SOFT action running.
  bool may_drive() const;
  // Once the driver is reachable, cancels every goal left on it by a previous adapter process.
  void clear_stale_goals();
  // Driver can take a goal: server up, driver not lost, stale goals cleared.
  bool driver_ready() const;
  void send_step(const RouteStep& step);
  void cancel_step();
  void on_step_response(uint64_t token, const StepGoalHandle::SharedPtr& handle);
  void on_step_edge_entered(uint64_t token);
  void on_step_result(uint64_t token, const StepGoalHandle::WrappedResult& result);
  // Incoming edge of the step entered: order and edge actions follow, once per edge.
  void enter_step_edge(RouteStep& step);
  // Node of the step reached: incoming edge completed, node popped, node actions triggered.
  void complete_step(RouteStep& step, double distance_driven);
  // Driver gave up the step's order: navigationError FATAL published once if failed, then order dropped.
  void drop_order(const RouteStep& step, const std::string& reason, bool failed);
  // paused = pause requested and no step in flight; completes startPause/stopPause on change.
  void update_paused();

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

  // ── Per-action commands on ~/action_command ───────────────────────────────
  void send_action_command(uint8_t command, const std::string& action_id);

  // ── Errors ─────────────────────────────────────────────────────────────────
  void replace_adapter_error(const vda5050::Error& error);
  void upsert_driver_error(const vda5050::Error& error);
  void clear_errors_by_type(const std::string& error_type);
  // Remove validationErrors raised for messages on topic.
  void clear_validation_errors(const std::string& topic);
  void report_validation_error(const std::string& topic, const std::string& payload,
                               const std::string& reason);
  void update_fatal_state();

  // ── State machine sync ─────────────────────────────────────────────────────
  void sync_order_activity();
  void sync_action_blocking();
  void log_mode_change();

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
  double      position_publish_min_interval_{1.0};
  double      hard_action_pause_timeout_{30.0};
  double      event_loop_period_{0.01};
  bool        strict_mode_{false};
  int64_t     new_base_request_min_base_nodes_{2};
  int64_t     max_finished_instant_actions_{50};
  double      driver_status_max_lease_{10.0};  // longest driver_status liveliness lease accepted (s), 0 = not tracked

  // ── Core components ────────────────────────────────────────────────────────
  std::unique_ptr<MqttClient>    mqtt_client_;
  std::unique_ptr<AdapterStateMachine> state_machine_;
  std::unique_ptr<OrderManager>  order_manager_;
  std::unique_ptr<ActionManager> action_manager_;
  std::string                    action_state_order_id_;
  vda5050::Factsheet             factsheet_;  // built once from params, published on connect

  // ── Work queued by the MQTT thread (guarded by inbound_mutex_) ────────────
  std::mutex                               inbound_mutex_;
  std::deque<std::function<void()>>        inbound_;
  bool                                     state_dirty_{false};
  AdapterMode                              logged_mode_{AdapterMode::INITIALIZING};

  // ── Route execution ────────────────────────────────────────────────────────
  std::optional<InFlightStep>              step_;
  uint64_t                                 step_token_{0};
  bool                                     pause_requested_{false};
  std::string                              driver_session_;  // session id of the driver, empty until known
  bool                                     driver_lost_{false};  // driver_status liveliness lost
  bool                                     stale_goals_cancel_sent_{false};
  bool                                     stale_goals_cleared_{false};  // cancel of all earlier goals answered

  // ── Robot state ────────────────────────────────────────────────────────────
  vda5050::AgvPosition          agv_position_;
  bool                          agv_position_set_{false};
  std::chrono::steady_clock::time_point last_position_publish_{};
  vda5050::Velocity             velocity_;
  bool                          velocity_set_{false};
  vda5050::BatteryState         battery_state_;
  vda5050::SafetyState          safety_state_;
  std::vector<vda5050::Load>    loads_;
  std::vector<vda5050::Error>   errors_;
  bool                          driving_{false};
  bool                          paused_{false};
  vda5050::OperatingMode        operating_mode_{vda5050::OperatingMode::AUTOMATIC};

  // ── Header ID counters per topic ──────────────────────────────────────────
  // VDA5050 §7.1: headerId must increment monotonically per message type.
  mutable std::unordered_map<std::string, uint32_t>     header_ids_;

  // ── ROS2 publishers / action client  (adapter → robot) ────────────────────
  rclcpp_action::Client<NavigateToNode>::SharedPtr                step_client_;
  rclcpp::Publisher<vda5050_msgs::msg::Action>::SharedPtr         action_execute_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::ActionCommand>::SharedPtr  action_command_pub_;

  // ── ROS2 publishers  (local status for on-robot tools) ────────────────────
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr               driving_pub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr               paused_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::NodeState>::SharedPtr      node_reached_pub_;
  rclcpp::Publisher<vda5050_msgs::msg::Error>::SharedPtr          error_pub_;

  // ── ROS2 subscribers  (robot → adapter) ───────────────────────────────────
  rclcpp::Subscription<vda5050_msgs::msg::AgvPosition>::SharedPtr  agv_pos_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Velocity>::SharedPtr     velocity_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::BatteryState>::SharedPtr battery_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::DriverStatus>::SharedPtr driver_status_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::ActionState>::SharedPtr  action_state_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Error>::SharedPtr        error_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::SafetyState>::SharedPtr  safety_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr           op_mode_sub_;
  rclcpp::Subscription<vda5050_msgs::msg::Load>::SharedPtr         load_sub_;
  rclcpp::Subscription<std_msgs::msg::Float64>::SharedPtr          distance_since_last_node_sub_;

  // ── Timers ─────────────────────────────────────────────────────────────────
  rclcpp::TimerBase::SharedPtr event_timer_;
  rclcpp::TimerBase::SharedPtr state_timer_;
  rclcpp::TimerBase::SharedPtr visualization_timer_;
  rclcpp::TimerBase::SharedPtr action_timeout_timer_;
};

}  // namespace vda5050_adapter
