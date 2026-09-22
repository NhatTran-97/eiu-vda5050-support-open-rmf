#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter::protocol {

// Factsheet capabilities used to choose blocking types and check actions.
class ParsedFactsheet
{
public:
  ParsedFactsheet() = default;
  explicit ParsedFactsheet(const nlohmann::json& raw);

  std::string series_name;
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
  bool supports_action(const std::string& action_type) const;

  // Use the preferred blocking type when supported, or the first declared type.
  std::string blocking_type_for(const std::string& action_type,  const std::string& preferred = "HARD") const;

  // Whether any capability was read from the factsheet.
  bool has_content() const;
};

// Actions every AGV must accept; the factsheet never blocks them.
bool is_core_action(const std::string& action_type);

enum class ActionCheck
{
  allowed,
  warn,
  reject,
};

struct ActionVerdict
{
  ActionCheck result = ActionCheck::allowed;
  std::string message;
};

// Check that the AGV declared an instant action; custom actions it lacks are rejected.
ActionVerdict check_instant_action(const std::string& action_type, const std::optional<ParsedFactsheet>& factsheet);

// Conflicts between a new instant action and what the AGV is doing: a blocking
// action sent while it drives, or an action sent during a running HARD-only action.
std::vector<std::string> action_conflicts(const std::string& action_type,
                                          const std::string& blocking_type,
                                          bool driving,
                                          const std::vector<nlohmann::json>& action_states,
                                          const std::optional<ParsedFactsheet>& factsheet);

// A hard violation stops the order from being sent; a soft one is only reported.
enum class Severity
{
  hard,
  soft,
};

struct Violation
{
  Severity severity;
  std::string message;
};

// Facts about the base+destination order about to be published.
struct OrderShape
{
  std::string map_id;
  std::array<double, 3> base_pose{};
  std::array<double, 3> dest_pose{};
  std::optional<double> seconds_since_last_order;
};

// Check an order against the AGV's factsheet limits and known maps.
std::vector<Violation> check_order(const OrderShape& order, const std::optional<ParsedFactsheet>& factsheet, const std::vector<std::string>& known_maps);

// True when any violation in the list is hard.
bool has_hard_violation(const std::vector<Violation>& violations);

}  // namespace vda5050_fleet_adapter::protocol
