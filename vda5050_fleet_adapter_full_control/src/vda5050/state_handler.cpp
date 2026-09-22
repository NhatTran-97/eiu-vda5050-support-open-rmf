#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

template <typename T>
std::optional<T> get_opt(const nlohmann::json &j, const char *key)
{
    if (j.contains(key) && !j.at(key).is_null())
    {
        return j.at(key).get<T>();
    }
    return std::nullopt;
}

std::vector<nlohmann::json> get_array(const nlohmann::json &j, const char *key)
{
    std::vector<nlohmann::json> out;
    if (j.contains(key) && j.at(key).is_array())
    {
        for (const auto &e : j.at(key))
        {
            out.push_back(e);
        }
    }
    return out;
}

// Parse the velocity vector shared by state and visualization messages.
std::optional<Velocity> parse_velocity(const nlohmann::json &raw)
{
    if (!raw.contains("velocity") || !raw["velocity"].is_object())
    {
        return std::nullopt;
    }
    const auto &v = raw["velocity"];
    Velocity parsed;
    parsed.vx = v.value("vx", 0.0);
    parsed.vy = v.value("vy", 0.0);
    parsed.omega = v.value("omega", 0.0);
    return parsed;
}

// Read `count` decimal digits of `text` starting at `pos`.
bool read_digits(const std::string &text, std::size_t pos, std::size_t count, int &value)
{
    if (pos + count > text.size())
    {
        return false;
    }
    int parsed = 0;
    for (std::size_t i = pos; i < pos + count; ++i)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return false;
        }
        parsed = parsed * 10 + (text[i] - '0');
    }
    value = parsed;
    return true;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar.
std::int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

}  // namespace

std::optional<std::int64_t> parse_timestamp_ms(const std::string &text)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (text.size() < 20 || !read_digits(text, 0, 4, year) || text[4] != '-' || !read_digits(text, 5, 2, month) ||
        text[7] != '-' || !read_digits(text, 8, 2, day) || text[10] != 'T' || !read_digits(text, 11, 2, hour) ||
        text[13] != ':' || !read_digits(text, 14, 2, minute) || text[16] != ':' || !read_digits(text, 17, 2, second))
    {
        return std::nullopt;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
    {
        return std::nullopt;
    }

    std::size_t pos = 19;
    std::int64_t millis = 0;
    if (text[pos] == '.')
    {
        ++pos;
        int places = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
        {
            if (places < 3)
            {
                millis = millis * 10 + (text[pos] - '0');
                ++places;
            }
            ++pos;
        }
        if (places == 0)
        {
            return std::nullopt;
        }
        for (; places < 3; ++places)
        {
            millis *= 10;
        }
    }
    if (pos + 1 != text.size() || text[pos] != 'Z')
    {
        return std::nullopt;
    }

    const std::int64_t seconds = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
                                 std::int64_t{hour} * 3600 + std::int64_t{minute} * 60 + second;
    return seconds * 1000 + millis;
}

double Velocity::speed() const
{
    return std::hypot(vx, vy);
}

bool is_terminal_action_status(const std::string &status)
{
    return status == "FINISHED" || status == "FAILED";
}

bool SafetyState::triggered() const
{
    return field_violation || (!e_stop.empty() && e_stop != "NONE");
}

ParsedState::ParsedState(const nlohmann::json &raw)
{
    if (raw.contains("headerId") && raw["headerId"].is_number_integer())
    {
        const auto id = raw["headerId"].get<std::int64_t>();
        if (id >= 0 && id <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            header_id = static_cast<std::uint32_t>(id);
        }
    }
    if (raw.contains("timestamp") && raw["timestamp"].is_string())
    {
        timestamp_ms = parse_timestamp_ms(raw["timestamp"].get<std::string>());
    }

    if (raw.contains("agvPosition") && raw["agvPosition"].is_object())
    {
        const auto &pos = raw["agvPosition"];
        x = get_opt<double>(pos, "x");
        y = get_opt<double>(pos, "y");
        theta = get_opt<double>(pos, "theta");
        map_id = pos.value("mapId", std::string{});
        position_initialized = pos.value("positionInitialized", false);
        localization_score = get_opt<double>(pos, "localizationScore");
    }

    if (raw.contains("batteryState") && raw["batteryState"].is_object())
    {
        const auto &battery = raw["batteryState"];
        if (const auto charge = get_opt<double>(battery, "batteryCharge"))
        {
            battery_soc = *charge / 100.0;
        }
        charging = battery.value("charging", false);
    }

    velocity = parse_velocity(raw);

    if (raw.contains("safetyState") && raw["safetyState"].is_object())
    {
        const auto &s = raw["safetyState"];
        safety_state.e_stop = s.value("eStop", std::string{"NONE"});
        safety_state.field_violation = s.value("fieldViolation", false);
    }

    order_id = raw.value("orderId", std::string{});
    order_update_id = get_opt<std::uint32_t>(raw, "orderUpdateId");
    zone_set_id = raw.value("zoneSetId", std::string{});
    last_node_id = raw.value("lastNodeId", std::string{});
    last_node_sequence_id = get_opt<std::uint32_t>(raw, "lastNodeSequenceId");
    driving = raw.value("driving", false);
    paused = raw.value("paused", false);
    new_base_request = raw.value("newBaseRequest", false);
    distance_since_last_node = get_opt<double>(raw, "distanceSinceLastNode");
    operating_mode = raw.value("operatingMode", std::string{"AUTOMATIC"});

    node_states = get_array(raw, "nodeStates");
    edge_states = get_array(raw, "edgeStates");
    action_states = get_array(raw, "actionStates");
    errors = get_array(raw, "errors");
    information = get_array(raw, "information");
    loads = get_array(raw, "loads");
    maps = get_array(raw, "maps");
}

bool ParsedState::has_position() const
{
    return x.has_value() && y.has_value() && theta.has_value() && position_initialized;
}

bool ParsedState::operable() const
{
    return operating_mode == "AUTOMATIC" || operating_mode == "SEMIAUTOMATIC";
}

std::string ParsedState::first_fatal_error() const
{
    for (const auto &e : errors)
    {
        if (!e.is_object() || e.value("errorLevel", std::string{}) != "FATAL")
        {
            continue;
        }
        const std::string type = e.value("errorType", std::string{});
        return type.empty() ? "(unnamed FATAL error)" : type;
    }
    return {};
}

std::optional<std::string> ParsedState::action_status(const std::string &action_id) const
{
    for (const auto &a : action_states)
    {
        if (a.value("actionId", std::string{}) == action_id)
        {
            return a.value("actionStatus", std::string{});
        }
    }
    return std::nullopt;
}

bool ParsedState::actions_settled(const std::vector<std::string> &action_ids) const
{
    for (const auto &id : action_ids)
    {
        const auto status = action_status(id);
        if (status.has_value() && !is_terminal_action_status(*status))
        {
            return false;
        }
    }
    return true;
}

bool ParsedState::order_finished(const std::string &expected_order_id,
                                 const std::string &target_node_id,
                                 const std::vector<std::string> &order_action_ids) const
{
    if (expected_order_id.empty() || order_id.empty() || order_id != expected_order_id)
    {
        return false;
    }
    if (!target_node_id.empty() && last_node_id != target_node_id)
    {
        return false;
    }
    return node_states.empty() && edge_states.empty() && !driving && actions_settled(order_action_ids);
}

ParsedVisualization::ParsedVisualization(const nlohmann::json &raw)
{
    if (raw.contains("agvPosition") && raw["agvPosition"].is_object())
    {
        const auto &pos = raw["agvPosition"];
        x = get_opt<double>(pos, "x");
        y = get_opt<double>(pos, "y");
        theta = get_opt<double>(pos, "theta");
        map_id = pos.value("mapId", std::string{});
        position_initialized = pos.value("positionInitialized", false);
    }
    velocity = parse_velocity(raw);
}

bool ParsedVisualization::has_position() const
{
    return x.has_value() && y.has_value() && theta.has_value() && position_initialized;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
