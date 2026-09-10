#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"

#include <algorithm>

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

std::string get_string(const nlohmann::json &j, const char *key)
{
    if (j.contains(key) && j.at(key).is_string())
    {
        return j.at(key).get<std::string>();
    }
    return {};
}

std::optional<double> get_number(const nlohmann::json &j, const char *key)
{
    if (j.contains(key) && j.at(key).is_number())
    {
        return j.at(key).get<double>();
    }
    return std::nullopt;
}

std::vector<std::string> get_string_array(const nlohmann::json &j, const char *key)
{
    std::vector<std::string> out;
    if (j.contains(key) && j.at(key).is_array())
    {
        for (const auto &e : j.at(key))
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

ParsedFactsheet::ParsedFactsheet(const nlohmann::json &raw)
{
    if (!raw.is_object())
    {
        return;
    }

    if (raw.contains("typeSpecification") && raw["typeSpecification"].is_object())
    {
        const auto &t = raw["typeSpecification"];
        series_name = get_string(t, "seriesName");
        agv_kinematic = get_string(t, "agvKinematic");
        agv_class = get_string(t, "agvClass");
        localization_types = get_string_array(t, "localizationTypes");
        navigation_types = get_string_array(t, "navigationTypes");
    }

    if (raw.contains("physicalParameters") && raw["physicalParameters"].is_object())
    {
        const auto &p = raw["physicalParameters"];
        speed_min = get_number(p, "speedMin");
        speed_max = get_number(p, "speedMax");
    }

    if (raw.contains("protocolFeatures") && raw["protocolFeatures"].is_object())
    {
        const auto &f = raw["protocolFeatures"];
        if (f.contains("agvActions") && f["agvActions"].is_array())
        {
            for (const auto &a : f["agvActions"])
            {
                if (!a.is_object())
                {
                    continue;
                }
                const std::string type = get_string(a, "actionType");
                if (type.empty())
                {
                    continue;
                }
                agv_actions[type] = get_string_array(a, "blockingTypes");
            }
        }
    }
}

bool ParsedFactsheet::supports_action(const std::string &action_type) const
{
    return agv_actions.find(action_type) != agv_actions.end();
}

std::string ParsedFactsheet::blocking_type_for(const std::string &action_type,
                                               const std::string &preferred) const
{
    const auto it = agv_actions.find(action_type);
    if (it == agv_actions.end() || it->second.empty())
    {
        return preferred;
    }

    const auto &declared = it->second;
    if (std::find(declared.begin(), declared.end(), preferred) != declared.end())
    {
        return preferred;
    }
    return declared.front();
}

bool ParsedFactsheet::has_content() const
{
    return !series_name.empty() || !agv_kinematic.empty() || !agv_class.empty() ||
           !localization_types.empty() || !navigation_types.empty() ||
           speed_min.has_value() || speed_max.has_value() || !agv_actions.empty();
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
