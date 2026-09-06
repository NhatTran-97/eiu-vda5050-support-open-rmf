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

  enum class State { IDLE, NAVIGATING, EXECUTING_ACTION };

  RobotStateMachine(rclcpp::Logger logger, Vda5050Connector& connector,
                    std::string robot_name);

  /// RMF asks the robot to navigate to a single destination.
  void on_navigate(const EasyFullControl::Destination& destination,
                   EasyFullControl::CommandExecution execution);

  /// RMF asks the robot to stop the activity identified by `activity`.
  void on_stop(ConstActivityIdentifierPtr activity);

  /// RMF asks the robot to perform a custom action (e.g. dock).
  void on_action(const std::string& category, const nlohmann::json& description,
                 RobotUpdateHandle::ActionExecution execution);

  /// Drive transitions from the latest cached VDA5050 state and return the
  /// activity currently underway (to pass to EasyRobotUpdateHandle::update).
  ConstActivityIdentifierPtr on_state_update();

  State state();

  /// Derive a VDA5050 nodeId from an RMF destination (name -> wp_<idx> -> x_y).
  static std::string derive_node_id(const std::string& name,
                                    std::optional<std::size_t> graph_index,
                                    double x, double y);

private:
  rclcpp::Logger _logger;
  Vda5050Connector& _connector;
  std::string _name;

  std::mutex _mutex;
  State _state = State::IDLE;
  std::optional<EasyFullControl::CommandExecution> _nav_exec;
  std::optional<RobotUpdateHandle::ActionExecution> _action_exec;
  std::string _action_id;

  // Deadline bookkeeping for the current navigation. A navigation that never
  // completes leaves RMF waiting forever with no log, so we at least say so.
  std::chrono::steady_clock::time_point _nav_started{};
  std::chrono::steady_clock::time_point _nav_last_warn{};
};

}  // namespace vda5050_fleet_adapter
