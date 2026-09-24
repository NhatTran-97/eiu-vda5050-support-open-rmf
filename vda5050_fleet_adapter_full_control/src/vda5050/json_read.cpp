#include "vda5050_fleet_adapter_full_control/vda5050/json_read.hpp"

#include <cmath>
#include <limits>

namespace vda5050_fleet_adapter_full_control::vda5050 {

namespace {

// The value of `key` in `object`, or null when there is none.
const nlohmann::json *field(const nlohmann::json &object, const char *key)
{
    if (!object.is_object())
    {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

}  // namespace

std::optional<std::string> read_string(const nlohmann::json &object, const char *key)
{
    const auto *value = field(object, key);
    if (!value || !value->is_string())
    {
        return std::nullopt;
    }
    return value->get<std::string>();
}

std::optional<bool> read_bool(const nlohmann::json &object, const char *key)
{
    const auto *value = field(object, key);
    if (!value || !value->is_boolean())
    {
        return std::nullopt;
    }
    return value->get<bool>();
}

std::optional<double> read_number(const nlohmann::json &object, const char *key)
{
    const auto *value = field(object, key);
    if (!value || !value->is_number())
    {
        return std::nullopt;
    }
    const double number = value->get<double>();
    return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

std::optional<std::uint32_t> read_uint32(const nlohmann::json &object, const char *key)
{
    const auto *value = field(object, key);
    if (!value)
    {
        return std::nullopt;
    }
    constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
    if (value->is_number_unsigned())
    {
        const auto number = value->get<std::uint64_t>();
        return number <= kMax ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(number)) : std::nullopt;
    }
    if (value->is_number_integer())
    {
        const auto number = value->get<std::int64_t>();
        return number >= 0 && number <= static_cast<std::int64_t>(kMax)
                   ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(number))
                   : std::nullopt;
    }
    if (value->is_number_float())
    {
        const double number = value->get<double>();
        if (std::isfinite(number) && number >= 0.0 && number <= static_cast<double>(kMax) && std::floor(number) == number)
        {
            return static_cast<std::uint32_t>(number);
        }
    }
    return std::nullopt;
}

std::vector<nlohmann::json> read_objects(const nlohmann::json &object, const char *key,
                                         std::initializer_list<const char *> string_fields)
{
    std::vector<nlohmann::json> out;
    const auto *array = field(object, key);
    if (!array || !array->is_array())
    {
        return out;
    }
    for (const auto &element : *array)
    {
        if (!element.is_object())
        {
            continue;
        }
        nlohmann::json cleaned = element;
        for (const char *name : string_fields)
        {
            const auto it = cleaned.find(name);
            if (it != cleaned.end() && !it->is_string())
            {
                cleaned.erase(it);
            }
        }
        out.push_back(std::move(cleaned));
    }
    return out;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
