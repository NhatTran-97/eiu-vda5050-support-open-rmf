#pragma once

#include <memory>
#include <string>

#include <rclcpp/logger.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>

#include "vda5050_fleet_adapter/robot_state_machine.hpp"
#include "vda5050_fleet_adapter/vda5050_connector.hpp"

namespace vda5050_fleet_adapter {

/// Bridges ONE EasyFullControl robot to the VDA5050 connector via the state
/// machine. Owns the per-robot RobotStateMachine and the EasyRobotUpdateHandle.
class RobotAdapter
{
public:
  using EasyFullControl = rmf_fleet_adapter::agv::EasyFullControl;

  RobotAdapter(rclcpp::Logger logger, std::string name,
               Vda5050Connector& connector);

  /// Build the navigate/stop/action callbacks handed to EasyFullControl.
  EasyFullControl::RobotCallbacks make_callbacks();

  /// Set once the robot has been added to the RMF fleet.
  void set_update_handle(
    std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> handle);

  bool added() const { return static_cast<bool>(_update_handle); }

  /// Push the latest AGV state into RMF (called every update tick).
  void update(const EasyFullControl::RobotState& state);

private:
  rclcpp::Logger _logger;
  std::string _name;
  RobotStateMachine _sm;
  std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> _update_handle;
};

}  // namespace vda5050_fleet_adapter
