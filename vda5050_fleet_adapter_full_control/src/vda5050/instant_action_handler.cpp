#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

nlohmann::json build_cancel_order(int header_id, const std::string &manufacturer,
                                  const std::string &serial,
                                  const std::string &blocking_type)
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(cancel_order_action("", blocking_type));
    return make_instant_actions(header_id, manufacturer, serial, actions);
}

nlohmann::json build_state_request(int header_id, const std::string &manufacturer,
                                   const std::string &serial,
                                   const std::string &blocking_type)
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(make_action("stateRequest", blocking_type, "", {}));
    return make_instant_actions(header_id, manufacturer, serial, actions);
}

nlohmann::json build_start_pause(int header_id, const std::string &manufacturer,
                                 const std::string &serial,
                                 const std::string &blocking_type)
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(make_action("startPause", blocking_type, "", {}));
    return make_instant_actions(header_id, manufacturer, serial, actions);
}

nlohmann::json build_stop_pause(int header_id, const std::string &manufacturer,
                                const std::string &serial,
                                const std::string &blocking_type)
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(make_action("stopPause", blocking_type, "", {}));
    return make_instant_actions(header_id, manufacturer, serial, actions);
}

InstantActionRequest build_instant_action(
    int header_id, const std::string &manufacturer, const std::string &serial,
    const std::string &action_type,
    const std::vector<std::pair<std::string, std::string>> &parameters,
    const std::string &blocking_type)
{
    const auto action = make_action(action_type, blocking_type, "", parameters);

    InstantActionRequest result;
    result.action_id = action.value("actionId", std::string{});

    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(action);
    result.message = make_instant_actions(header_id, manufacturer, serial, actions);

    return result;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
