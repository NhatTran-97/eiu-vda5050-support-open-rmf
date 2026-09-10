#ifndef FACTSHEET_HANDLER_HPP
#define FACTSHEET_HANDLER_HPP

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// The subset of a VDA5050 'factsheet' message this adapter acts on: what the
// AGV says it is, and which actions (with which blocking types) it accepts.
//
// Parsing is tolerant on purpose -- a factsheet is informational, and a robot
// that omits or mistypes a field should not take the adapter down. Every
// field that is absent, null, or of the wrong JSON type is left at its
// default (empty string/vector/map, nullopt), and the constructor does not
// throw. Use has_content() to tell "nothing usable was parsed" apart from
// "the AGV genuinely declared nothing".
class ParsedFactsheet
{
public:
    ParsedFactsheet() = default;
    explicit ParsedFactsheet(const nlohmann::json &raw);

    // typeSpecification
    std::string series_name;
    std::string agv_kinematic;
    std::string agv_class;
    std::vector<std::string> localization_types;
    std::vector<std::string> navigation_types;

    // physicalParameters -- the speed envelope, for sanity-checking the speed
    // limits this adapter puts on an order's edges.
    std::optional<double> speed_min;
    std::optional<double> speed_max;

    // protocolFeatures.agvActions: actionType -> the blockingTypes declared
    // for it. An actionType absent from this map was not declared at all;
    // a present-but-empty vector means it was declared without blockingTypes.
    std::map<std::string, std::vector<std::string>> agv_actions;

    // True when the AGV declared this actionType in protocolFeatures.agvActions.
    bool supports_action(const std::string &action_type) const;

    // The blockingType to send for `action_type`:
    // - `preferred` when the AGV declares it,
    // - otherwise the AGV's first declared blockingType,
    // - otherwise `preferred` unchanged (action undeclared, or declared with
    //   no blockingTypes -- the caller decides whether to send it anyway).
    std::string blocking_type_for(const std::string &action_type,
                                  const std::string &preferred = "HARD") const;

    // False when the message carried none of the fields above -- e.g. an
    // empty object, or a payload that isn't a factsheet at all.
    bool has_content() const;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // FACTSHEET_HANDLER_HPP
