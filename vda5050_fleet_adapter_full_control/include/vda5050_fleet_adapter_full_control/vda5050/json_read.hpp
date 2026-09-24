#ifndef JSON_READ_HPP
#define JSON_READ_HPP

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Readers for fields of AGV messages: an absent field, a non-object parent or a value of another JSON type reads as nullopt.
std::optional<std::string> read_string(const nlohmann::json &object, const char *key);
std::optional<bool> read_bool(const nlohmann::json &object, const char *key);

// A finite number.
std::optional<double> read_number(const nlohmann::json &object, const char *key);

// A whole number from 0 to 4294967295.
std::optional<std::uint32_t> read_uint32(const nlohmann::json &object, const char *key);

// The object elements of an array field; in each, the listed fields that are not strings are removed.
std::vector<nlohmann::json> read_objects(const nlohmann::json &object, const char *key,
                                         std::initializer_list<const char *> string_fields = {});

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // JSON_READ_HPP
