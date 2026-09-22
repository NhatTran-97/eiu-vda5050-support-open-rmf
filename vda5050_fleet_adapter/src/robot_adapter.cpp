#include "vda5050_fleet_adapter/robot_adapter.hpp"

#include <utility>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter {

RobotAdapter::RobotAdapter(rclcpp::Logger logger, std::string name,Vda5050Connector& connector) 
                                  :  _logger(logger), _name(std::move(name)),_sm(logger, connector, _name)
{
}

RobotAdapter::EasyFullControl::RobotCallbacks RobotAdapter::make_callbacks()
{
  using EasyFullControl = rmf_fleet_adapter::agv::EasyFullControl;
  using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;

  return EasyFullControl::RobotCallbacks([this](EasyFullControl::Destination destination,
           EasyFullControl::CommandExecution execution) 
    {
      _sm.on_navigate(destination, std::move(execution));
    },
    [this](RobotUpdateHandle::ConstActivityIdentifierPtr activity) 
    {
      _sm.on_stop(std::move(activity));
    },

    [this](const std::string& category, const nlohmann::json& description, RobotUpdateHandle::ActionExecution execution) 
    {
      _sm.on_action(category, description, std::move(execution));
    });
}

void RobotAdapter::set_update_handle(
  std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> handle)
{
  _update_handle = std::move(handle);
}

void RobotAdapter::update(const EasyFullControl::RobotState& state)
{
  if (!_update_handle)
  {
    return;
  }
    
  const auto activity = _sm.on_state_update();
  _update_handle->update(state, activity);
}

void RobotAdapter::apply_readiness(const Readiness& readiness)
{
  if (!_update_handle)
  {
    return;
  }
  if (_ready == readiness.ready)
  {
    return;
  }

  // RMF finishes registering the robot asynchronously; retry on the next update.
  const auto handle = _update_handle->more();
  if (!handle)
  {
    return;
  }

  using Commission = rmf_fleet_adapter::agv::RobotUpdateHandle::Commission;
  handle->set_commission(readiness.ready ? Commission() : Commission::decommission());

  if (readiness.ready)
  {
    if (_ready.has_value())
    {
      RCLCPP_INFO(_logger, "[%s] available again -- recommissioned with RMF", _name.c_str());
    }
  }
  else
  {
    RCLCPP_WARN(_logger, "[%s] %s -- decommissioned, RMF will not dispatch new tasks to it", _name.c_str(), readiness.reason.c_str());
  }
  _ready = readiness.ready;
}

}  // namespace vda5050_fleet_adapter
