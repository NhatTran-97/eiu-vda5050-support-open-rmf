#include "vda5050_fleet_adapter/factsheet.hpp"

#include <algorithm>
#include <iterator>

namespace vda5050_fleet_adapter::protocol {

namespace {

std::string get_string(const nlohmann::json& j, const char* key)
{
  if (j.contains(key) && j.at(key).is_string())
  {
    return j.at(key).get<std::string>();
  }
  return {};
}

std::optional<double> get_number(const nlohmann::json& j, const char* key)
{
  if (j.contains(key) && j.at(key).is_number())
  {
    return j.at(key).get<double>();
  }
  return std::nullopt;
}

std::vector<std::string> get_string_array(const nlohmann::json& j, const char* key)
{
  std::vector<std::string> out;
  if (j.contains(key) && j.at(key).is_array())
  {
    for (const auto& e : j.at(key))
    {
      if (e.is_string())
      {
        out.push_back(e.get<std::string>());
      }
    }
  }
  return out;
}

}  // namespace

ParsedFactsheet::ParsedFactsheet(const nlohmann::json& raw)
{
  if (!raw.is_object())
  {
    return;
  }

  if (raw.contains("typeSpecification") && raw["typeSpecification"].is_object())
  {
    series_name = get_string(raw["typeSpecification"], "seriesName");
  }

  if (raw.contains("physicalParameters") && raw["physicalParameters"].is_object())
  {
    speed_max = get_number(raw["physicalParameters"], "speedMax");
  }

  if (raw.contains("protocolFeatures") && raw["protocolFeatures"].is_object())
  {
    const auto& features = raw["protocolFeatures"];
    if (features.contains("agvActions") && features["agvActions"].is_array())
    {
      for (const auto& a : features["agvActions"])
      {
        const std::string type = a.is_object() ? get_string(a, "actionType") : std::string{};
        if (type.empty())
        {
          continue;
        }
        AgvAction info;
        info.blocking_types = get_string_array(a, "blockingTypes");
        info.scopes = get_string_array(a, "actionScopes");
        agv_actions[type] = std::move(info);
      }
    }
  }
}

bool ParsedFactsheet::supports_action(const std::string& action_type) const
{
  return agv_actions.find(action_type) != agv_actions.end();
}

std::string ParsedFactsheet::blocking_type_for(const std::string& action_type,
                                               const std::string& preferred) const
{
  const auto it = agv_actions.find(action_type);
  if (it == agv_actions.end() || it->second.blocking_types.empty())
  {
    return preferred;
  }

  const auto& declared = it->second.blocking_types;
  if (std::find(declared.begin(), declared.end(), preferred) != declared.end())
  {
    return preferred;
  }
  return declared.front();
}

bool ParsedFactsheet::has_content() const
{
  return !series_name.empty() || speed_max.has_value() || !agv_actions.empty();
}

bool is_core_action(const std::string& action_type)
{
  static const char* const kCore[] = {"cancelOrder", "startPause", "stopPause",
                                      "stateRequest", "initPosition", "factsheetRequest"};
  return std::any_of(std::begin(kCore), std::end(kCore), [&](const char* core) { return action_type == core; });
}

ActionVerdict check_instant_action(const std::string& action_type,
                                   const std::optional<ParsedFactsheet>& factsheet)
{
  if (!factsheet.has_value() || factsheet->supports_action(action_type))
  {
    return {};
  }
  ActionVerdict verdict;
  verdict.result = is_core_action(action_type) ? ActionCheck::warn : ActionCheck::reject;
  verdict.message = "action '" + action_type + "' is not in the AGV's factsheet (protocolFeatures.agvActions)";
  return verdict;
}

std::vector<std::string> action_conflicts(const std::string& action_type,
                                          const std::string& blocking_type,
                                          bool driving,
                                          const std::vector<nlohmann::json>& action_states,
                                          const std::optional<ParsedFactsheet>& factsheet)
{
  std::vector<std::string> out;

  if (driving && blocking_type != "NONE")
  {
    out.push_back("sending '" + action_type + "' (blockingType " + blocking_type +
                  ") while the AGV is still driving -- " + blocking_type +
                  " actions expect it stationary and may be queued or rejected");
  }

  if (!factsheet.has_value())
  {
    return out;
  }

  // A running HARD-only action has exclusive control.
  for (const auto& running : action_states)
  {
    const std::string status = running.value("actionStatus", std::string{});
    if (status.empty() || status == "FINISHED" || status == "FAILED")
    {
      continue;
    }
    const std::string running_type = running.value("actionType", std::string{});
    if (running_type.empty() || running_type == action_type)
    {
      continue;
    }
    const auto it = factsheet->agv_actions.find(running_type);
    if (it != factsheet->agv_actions.end() && it->second.blocking_types.size() == 1 && it->second.blocking_types.front() == "HARD")
    {
      out.push_back("sending '" + action_type + "' while '" + running_type + "' (action " +
                    running.value("actionId", std::string{}) + ") is still " + status +
                    " and only ever runs HARD -- it holds exclusivity, this action may be "
                    "queued or rejected");
    }
  }
  return out;
}

}  // namespace vda5050_fleet_adapter::protocol
