#ifndef ROBOT_HPP
#define ROBOT_HPP

#include <memory>
#include <string>

#include <rclcpp/logger.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/robot_activity_state_machine.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

namespace vda5050_fleet_adapter_full_control::core {

class RobotAdapter
{
public:
    using EasyFullControl = rmf_fleet_adapter::agv::EasyFullControl;

    RobotAdapter(rclcpp::Logger logger, std::string name, rmf::Connector &connector);

    // The returned callbacks capture `this` and are retained by RMF for as
    // long as this robot stays registered in the fleet -- this RobotAdapter
    // must outlive that registration.
    EasyFullControl::RobotCallbacks make_callbacks();

    void set_update_handle(std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> handle);

    bool added() const { return static_cast<bool>(_update_handle); }

    void update(const EasyFullControl::RobotState &state);

private:
    rclcpp::Logger _logger;
    std::string _name;
    rmf::RobotActivityStateMachine _sm;
    std::shared_ptr<EasyFullControl::EasyRobotUpdateHandle> _update_handle;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // ROBOT_HPP
