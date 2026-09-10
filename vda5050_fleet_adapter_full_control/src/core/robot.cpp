#include "vda5050_fleet_adapter_full_control/core/robot.hpp"

#include <utility>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter_full_control::core {

RobotAdapter::RobotAdapter(rclcpp::Logger logger, std::string name, rmf::Connector &connector)
  : _logger(logger), _name(std::move(name)), _sm(logger, connector, _name)
{
}

RobotAdapter::EasyFullControl::RobotCallbacks RobotAdapter::make_callbacks()
{
    using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;

    return EasyFullControl::RobotCallbacks(
        [this](EasyFullControl::Destination destination, EasyFullControl::CommandExecution execution)
        {
            _sm.on_navigate(destination, std::move(execution));
        },
        [this](RobotUpdateHandle::ConstActivityIdentifierPtr activity)
        {
            _sm.on_stop(std::move(activity));
        },
        [this](const std::string &category, const nlohmann::json &description,
               RobotUpdateHandle::ActionExecution execution)
        {
            _sm.on_action(category, description, std::move(execution));
        })
        .with_localization(
            [this](EasyFullControl::Destination estimate,
                   EasyFullControl::CommandExecution execution)
            {
                _sm.on_localize(estimate, std::move(execution));
            });
}

void RobotAdapter::set_update_handle(std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> handle)
{
    _update_handle = std::move(handle);
}

void RobotAdapter::set_online(bool online)
{
    if (!_update_handle || _online == online)
    {
        return;
    }
    _online = online;

    const auto handle = _update_handle->more();
    if (!handle)
    {
        return;
    }

    using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;
    if (online)
    {
        handle->set_commission(RobotUpdateHandle::Commission());
        RCLCPP_INFO(_logger, "[%s] state is flowing again -- recommissioned with RMF",
                    _name.c_str());
    }
    else
    {
        handle->set_commission(RobotUpdateHandle::Commission::decommission());
        RCLCPP_WARN(_logger,
                    "[%s] no recent VDA5050 state -- decommissioned, RMF will not "
                    "dispatch new tasks to it",
                    _name.c_str());
    }
}

void RobotAdapter::update(const EasyFullControl::RobotState &state)
{
    if (!_update_handle)
    {
        return;
    }

    const auto activity = _sm.on_state_update();
    _update_handle->update(state, activity);
}

}  // namespace vda5050_fleet_adapter_full_control::core
