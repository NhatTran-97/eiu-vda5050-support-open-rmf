#pragma once

#include <memory>
#include <optional>
#include <string>

#include <rclcpp/logger.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>

#include "vda5050_fleet_adapter/readiness.hpp"
#include "vda5050_fleet_adapter/robot_state_machine.hpp"
#include "vda5050_fleet_adapter/vda5050_connector.hpp"

namespace vda5050_fleet_adapter {


class RobotAdapter
{
public:
  using EasyFullControl = rmf_fleet_adapter::agv::EasyFullControl;

  RobotAdapter(rclcpp::Logger logger, std::string name,Vda5050Connector& connector);

  EasyFullControl::RobotCallbacks make_callbacks();

  void set_update_handle(  std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> handle);

  bool added() const { return static_cast<bool>(_update_handle); }

  void update(const EasyFullControl::RobotState& state);

  // Decommission the robot in RMF while it is not ready, and recommission it when it is.
  void apply_readiness(const Readiness& readiness);

  // Whether a pause reported by the AGV is one the adapter asked for.
  bool pause_expected() { return _sm.pause_expected(); }

  // Operator pause and resume; each returns an error string, empty on success.
  std::string pause() { return _sm.pause(); }
  std::string resume() { return _sm.resume(); }

private:
  rclcpp::Logger _logger;
  std::string _name;
  RobotStateMachine _sm;
  std::optional<bool> _ready;
  std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> _update_handle;
};

}  // namespace vda5050_fleet_adapter
