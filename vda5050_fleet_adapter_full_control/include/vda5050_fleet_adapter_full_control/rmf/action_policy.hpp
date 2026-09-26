#ifndef ACTION_POLICY_HPP
#define ACTION_POLICY_HPP

#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::rmf {

// VDA5050 action sent for an RMF dock.
struct DockAction
{
    std::string action_type;
    nlohmann::json parameters = nlohmann::json::object();
};

// VDA5050 actions for RMF commands.
struct ActionPolicy
{
    // Dock name -> action; unlisted docks use their own name.
    std::map<std::string, DockAction> dock_actions;
    // startCharging at a charger, stopCharging before the next order.
    bool charge_at_chargers = false;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // ACTION_POLICY_HPP
