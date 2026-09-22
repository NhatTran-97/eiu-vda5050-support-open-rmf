#ifndef FACTSHEET_HANDLER_HPP
#define FACTSHEET_HANDLER_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Factsheet capabilities used to check actions and protocol limits.
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

    // physicalParameters speed envelope.
    std::optional<double> speed_min;
    std::optional<double> speed_max;
    std::optional<double> acceleration_max;

    // physicalParameters footprint in metres.
    std::optional<double> length;
    std::optional<double> width;

    // Supported blocking types and scopes for one AGV action.
    struct AgvAction
    {
        std::vector<std::string> blocking_types;
        std::vector<std::string> scopes;
    };

    // protocolFeatures.agvActions indexed by actionType.
    std::map<std::string, AgvAction> agv_actions;

    // protocolLimits.maxArrayLens values, when declared.
    std::optional<std::uint32_t> max_order_nodes;
    std::optional<std::uint32_t> max_order_edges;

    // protocolLimits.timing.minOrderInterval in seconds.
    std::optional<double> min_order_interval;

    // True when the AGV declared this actionType in protocolFeatures.agvActions.
    bool supports_action(const std::string &action_type) const;

    // Check an action scope when the factsheet declares one.
    bool supports_scope(const std::string &action_type, const std::string &scope) const;

    // Use the preferred blocking type when supported, or the first declared type.
    std::string blocking_type_for(const std::string &action_type, const std::string &preferred = "HARD") const;

    // Whether any supported capability was read from the factsheet.
    bool has_content() const;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // FACTSHEET_HANDLER_HPP
