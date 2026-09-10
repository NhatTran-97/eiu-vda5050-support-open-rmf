#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

nlohmann::json build_cancel_order(int header_id, const std::string &manufacturer,
                                  const std::string &serial)
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(cancel_order_action());
    return make_instant_actions(header_id, manufacturer, serial, actions);
}

nlohmann::json build_state_request(int header_id, const std::string &manufacturer,
                                   const std::string &serial)
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(make_action("stateRequest", "NONE", "", {}));
    return make_instant_actions(header_id, manufacturer, serial, actions);
}

InstantActionRequest build_instant_action(
    int header_id, const std::string &manufacturer, const std::string &serial,
    const std::string &action_type,
    const std::vector<std::pair<std::string, std::string>> &parameters)
{
    const auto action = make_action(action_type, "HARD", "", parameters);

    InstantActionRequest result;
    result.action_id = action.value("actionId", std::string{});

    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(action);
    result.message = make_instant_actions(header_id, manufacturer, serial, actions);

    return result;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
