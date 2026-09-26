#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

#include <cmath>
#include <cstdint>

#include "vda5050_fleet_adapter_full_control/vda5050/json_read.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

// The object at `key`, or null when it is absent or not an object.
const nlohmann::json *object_at(const nlohmann::json &raw, const char *key)
{
    if (!raw.is_object())
    {
        return nullptr;
    }
    const auto it = raw.find(key);
    return it != raw.end() && it->is_object() ? &*it : nullptr;
}

// Parse the velocity vector shared by state and visualization messages.
std::optional<Velocity> parse_velocity(const nlohmann::json &raw)
{
    const auto *v = object_at(raw, "velocity");
    if (!v)
    {
        return std::nullopt;
    }
    Velocity parsed;
    parsed.vx = read_number(*v, "vx").value_or(0.0);
    parsed.vy = read_number(*v, "vy").value_or(0.0);
    parsed.omega = read_number(*v, "omega").value_or(0.0);
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

ParsedState::ParsedState(const nlohmann::json &raw) : header_id(read_uint32(raw, "headerId"))
{
    if (const auto stamp = read_string(raw, "timestamp"))
    {
        timestamp_ms = parse_timestamp_ms(*stamp);
    }

    if (const auto *pos = object_at(raw, "agvPosition"))
    {
        x = read_number(*pos, "x");
        y = read_number(*pos, "y");
        theta = read_number(*pos, "theta");
        map_id = read_string(*pos, "mapId").value_or("");
        position_initialized = read_bool(*pos, "positionInitialized").value_or(false);
        localization_score = read_number(*pos, "localizationScore");
    }

    if (const auto *battery = object_at(raw, "batteryState"))
    {
        if (const auto charge = read_number(*battery, "batteryCharge"))
        {
            battery_soc = *charge / 100.0;
        }
        charging = read_bool(*battery, "charging").value_or(false);
    }

    velocity = parse_velocity(raw);

    if (const auto *s = object_at(raw, "safetyState"))
    {
        safety_state.e_stop = read_string(*s, "eStop").value_or("NONE");
        safety_state.field_violation = read_bool(*s, "fieldViolation").value_or(false);
    }

    order_id = read_string(raw, "orderId").value_or("");
    order_update_id = read_uint32(raw, "orderUpdateId");
    zone_set_id = read_string(raw, "zoneSetId").value_or("");
    last_node_id = read_string(raw, "lastNodeId").value_or("");
    last_node_sequence_id = read_uint32(raw, "lastNodeSequenceId");
    driving = read_bool(raw, "driving").value_or(false);
    paused = read_bool(raw, "paused").value_or(false);
    new_base_request = read_bool(raw, "newBaseRequest").value_or(false);
    distance_since_last_node = read_number(raw, "distanceSinceLastNode");
    operating_mode = read_string(raw, "operatingMode").value_or("AUTOMATIC");

    node_states = read_objects(raw, "nodeStates", {"nodeId"});
    edge_states = read_objects(raw, "edgeStates", {"edgeId"});
    action_states = read_objects(raw, "actionStates", {"actionId", "actionType", "actionStatus", "resultDescription"});
    errors = read_objects(raw, "errors", {"errorType", "errorLevel", "errorDescription"});
    information = read_objects(raw, "information", {"infoType", "infoLevel", "infoDescription"});
    loads = read_objects(raw, "loads", {"loadId", "loadType"});
    maps = read_objects(raw, "maps", {"mapId", "mapVersion", "mapStatus"});
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

std::vector<AgvError> ParsedState::agv_errors() const
{
    std::vector<AgvError> out;
    for (const auto &e : errors)
    {
        const std::string level = e.value("errorLevel", std::string{});
        if (level != "WARNING" && level != "FATAL")
        {
            continue;
        }
        const std::string type = e.value("errorType", std::string{});
        out.push_back({type.empty() ? "(unnamed error)" : type, level, e.value("errorDescription", std::string{})});
    }
    return out;
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
    if (const auto *pos = object_at(raw, "agvPosition"))
    {
        x = read_number(*pos, "x");
        y = read_number(*pos, "y");
        theta = read_number(*pos, "theta");
        map_id = read_string(*pos, "mapId").value_or("");
        position_initialized = read_bool(*pos, "positionInitialized").value_or(false);
    }
    velocity = parse_velocity(raw);
}

bool ParsedVisualization::has_position() const
{
    return x.has_value() && y.has_value() && theta.has_value() && position_initialized;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
