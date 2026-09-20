#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <rclcpp/logger.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>
#include <rmf_fleet_adapter/agv/RobotUpdateHandle.hpp>

#include "vda5050_fleet_adapter/vda5050_connector.hpp"

namespace vda5050_fleet_adapter {

class RobotStateMachine
{
public:
  using EasyFullControl = rmf_fleet_adapter::agv::EasyFullControl;
  using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;
  using ConstActivityIdentifierPtr =
    RobotUpdateHandle::ConstActivityIdentifierPtr;

  // HOLDING: RMF stopped the robot; its order stays paused until a new command or a timeout.
  enum class State { IDLE, NAVIGATING, EXECUTING_ACTION, HOLDING };

  RobotStateMachine(rclcpp::Logger logger, Vda5050Connector& connector, std::string robot_name);

  /// RMF asks the robot to navigate to a single destination.
  void on_navigate(const EasyFullControl::Destination& destination, EasyFullControl::CommandExecution execution);

  /// RMF asks the robot to stop the activity identified by `activity`.
  void on_stop(ConstActivityIdentifierPtr activity);

  /// RMF asks the robot to perform a custom action (e.g. dock).
  void on_action(const std::string& category, const nlohmann::json& description, RobotUpdateHandle::ActionExecution execution);

  /// Drive transitions from the latest cached VDA5050 state and return the
  /// activity currently underway (to pass to EasyRobotUpdateHandle::update).
  ConstActivityIdentifierPtr on_state_update();

  State state();

  /// Whether a pause reported by the AGV comes from the adapter's own hold or a resume it just sent, not from the operator.
  bool pause_expected();

  /// Operator pause and resume; each returns an error string, empty on success.
  std::string pause();
  std::string resume();

  /// Derive a VDA5050 nodeId from an RMF destination (name -> wp_<idx> -> x_y).
  static std::string derive_node_id(const std::string& name,  std::optional<std::size_t> graph_index,  double x, double y);

private:
  rclcpp::Logger _logger;
  Vda5050Connector& _connector;
  std::string _name;

  std::mutex _mutex;
  State _state = State::IDLE;
  std::optional<EasyFullControl::CommandExecution> _nav_exec;
  std::optional<RobotUpdateHandle::ActionExecution> _action_exec;
  std::string _action_id;

  std::optional<std::chrono::steady_clock::time_point> _hold_deadline;
  std::chrono::steady_clock::time_point _resume_grace_until{};
  bool _operator_paused = false;

  /// Cancel the held order (when `cancel`) and unpause unless the operator paused.
  void release_hold_locked(bool cancel);
  /// Send stopPause unless the operator paused the robot.
  void resume_unless_operator_paused_locked();

  // Start time and last warning time of the current navigation.
  std::chrono::steady_clock::time_point _nav_started{};
  std::chrono::steady_clock::time_point _nav_last_warn{};
};

}  // namespace vda5050_fleet_adapter
