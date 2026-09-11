#ifndef INSTANT_ACTION_HANDLER_HPP
#define INSTANT_ACTION_HANDLER_HPP

#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Builds an 'instantActions' message carrying a single cancelOrder action.
// Callers should use the blocking type declared by the AGV factsheet.
nlohmann::json build_cancel_order(int header_id, const std::string &manufacturer,
                                  const std::string &serial,
                                  const std::string &blocking_type = "HARD");

// Builds an 'instantActions' message carrying a single stateRequest action.
nlohmann::json build_state_request(int header_id, const std::string &manufacturer,
                                   const std::string &serial,
                                   const std::string &blocking_type = "NONE");

// Builds a startPause action that retains the active order for later resume.
nlohmann::json build_start_pause(int header_id, const std::string &manufacturer,
                                 const std::string &serial,
                                 const std::string &blocking_type = "NONE");

// Builds a stopPause action that resumes a paused order.
nlohmann::json build_stop_pause(int header_id, const std::string &manufacturer,
                                const std::string &serial,
                                const std::string &blocking_type = "NONE");

// Message payload and the generated action identifier.
struct InstantActionRequest
{
    nlohmann::json message;
    std::string action_id;
};

// Builds a custom instant action. Parameter value types are preserved.
InstantActionRequest build_instant_action(
    int header_id, const std::string &manufacturer, const std::string &serial, const std::string &action_type, 
                                    const nlohmann::json &parameters = nlohmann::json::object(),
                                    const std::string &blocking_type = "HARD");

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // INSTANT_ACTION_HANDLER_HPP
