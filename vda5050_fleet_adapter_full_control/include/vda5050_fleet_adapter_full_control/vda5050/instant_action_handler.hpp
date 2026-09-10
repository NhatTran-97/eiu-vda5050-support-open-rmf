#ifndef INSTANT_ACTION_HANDLER_HPP
#define INSTANT_ACTION_HANDLER_HPP

#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Builds an 'instantActions' message carrying a single cancelOrder action.
// blocking_type defaults to what this adapter sent before it consumed
// factsheets; callers that have one should pass the AGV's declared type
// (see ParsedFactsheet::blocking_type_for()).
nlohmann::json build_cancel_order(int header_id, const std::string &manufacturer,
                                  const std::string &serial,
                                  const std::string &blocking_type = "HARD");

// Builds an 'instantActions' message carrying a single stateRequest action.
nlohmann::json build_state_request(int header_id, const std::string &manufacturer,
                                   const std::string &serial,
                                   const std::string &blocking_type = "NONE");

// Builds an 'instantActions' message carrying a single startPause action:
// the AGV holds its current order and stops moving, keeping the order so it
// can be resumed with build_stop_pause() (unlike cancelOrder, which
// discards it).
nlohmann::json build_start_pause(int header_id, const std::string &manufacturer,
                                 const std::string &serial,
                                 const std::string &blocking_type = "NONE");

// Builds an 'instantActions' message carrying a single stopPause action:
// the AGV resumes the order it was holding after a startPause.
nlohmann::json build_stop_pause(int header_id, const std::string &manufacturer,
                                const std::string &serial,
                                const std::string &blocking_type = "NONE");

// Result of build_instant_action(): the message to publish, plus the
// actionId assigned to the action inside it (a fresh UUID unless the
// caller cares to inspect it -- see message_builder's make_action()).
struct InstantActionRequest
{
    nlohmann::json message;
    std::string action_id;
};

// Builds an 'instantActions' message carrying one action of any actionType.
// blocking_type defaults to "HARD" (what this adapter always sent before it
// consumed factsheets); callers that have a factsheet should pass the AGV's
// declared type instead. `parameters` is a flat key -> string map -- see
// make_action()'s own documented limitation on numeric/boolean values.
InstantActionRequest build_instant_action(int header_id, const std::string &manufacturer, const std::string &serial,
                                        const std::string &action_type,
                                        const std::vector<std::pair<std::string, std::string>> &parameters = {},
                                        const std::string &blocking_type = "HARD");

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // INSTANT_ACTION_HANDLER_HPP
