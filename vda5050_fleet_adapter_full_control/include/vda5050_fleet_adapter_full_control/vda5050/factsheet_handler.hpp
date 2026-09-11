#ifndef FACTSHEET_HANDLER_HPP
#define FACTSHEET_HANDLER_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Parsed subset of a VDA5050 factsheet used for capability checks. Unsupported
// or missing fields are ignored because factsheet data is advisory.
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

    // Checks an explicitly declared action scope. Missing declarations are
    // treated as unknown and therefore accepted.
    bool supports_scope(const std::string &action_type, const std::string &scope) const;

    // Selects the preferred blocking type when supported, otherwise the first
    // declared type. Returns `preferred` when no declaration is available.
    std::string blocking_type_for(const std::string &action_type, const std::string &preferred = "HARD") const;

    // True when at least one supported factsheet field was parsed.
    bool has_content() const;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // FACTSHEET_HANDLER_HPP
