#ifndef INSTANT_ACTION_HANDLER_HPP
#define INSTANT_ACTION_HANDLER_HPP

#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Builds an 'instantActions' message carrying a single cancelOrder action,
// matching Vda5050Connector::stop().
nlohmann::json build_cancel_order(int header_id, const std::string &manufacturer,
                                  const std::string &serial);

// Builds an 'instantActions' message carrying a single stateRequest action,
// matching Vda5050Connector::request_state().
nlohmann::json build_state_request(int header_id, const std::string &manufacturer,
                                   const std::string &serial);

// Result of build_instant_action(): the message to publish, plus the
// actionId assigned to the action inside it (a fresh UUID unless the
// caller cares to inspect it -- see message_builder's make_action()).
struct InstantActionRequest
{
    nlohmann::json message;
    std::string action_id;
};

// Builds an 'instantActions' message carrying one action of any
// actionType, always with blocking_type "HARD" (matching
// Vda5050Connector::execute_instant_action(), which never varied it).
// `parameters` is a flat key -> string map -- see make_action()'s own
// documented limitation on numeric/boolean parameter values.
InstantActionRequest build_instant_action(int header_id, const std::string &manufacturer, const std::string &serial,
                                        const std::string &action_type,
                                        const std::vector<std::pair<std::string, std::string>> &parameters = {});

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // INSTANT_ACTION_HANDLER_HPP
