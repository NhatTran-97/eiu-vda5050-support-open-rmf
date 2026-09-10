#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <optional>
#include <string>

#include <yaml-cpp/yaml.h>

#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"

namespace vda5050_fleet_adapter_full_control::core {

struct Args
{
    std::string config_file;
    std::string nav_graph;
};

Args parse_args(int argc, char **argv);

struct MqttConfig
{
    std::string broker_url;
    std::optional<std::string> username;
    std::optional<std::string> password;
};

struct RobotConfig
{
    std::string manufacturer = "unknown";
    std::string serial;
    rmf::Transform transform;
};

// Reads the `vda5050:` block of config_file once at construction. Throws
// (a yaml-cpp exception, or std::runtime_error from a semantic check below)
// if the file is missing, the `vda5050:` block itself is absent, a present
// field has the wrong type, or a value fails validation (empty identity,
// non-finite/out-of-range number, zero transform scale). A field that is
// simply absent *within* an existing vda5050: block falls back to the same
// defaults the old main.cpp hardcoded.
class Config
{
public:
    explicit Config(const std::string &config_file);

    const std::string &interface_name() const { return _interface_name; }
    double update_rate_hz() const { return _update_rate_hz; }
    const MqttConfig &mqtt() const { return _mqtt; }

    // manufacturer defaults to "unknown", serial defaults to `name`,
    // transform defaults to identity, when the robot has no entry (or a
    // partial one) under vda5050.robots.<name>.
    RobotConfig robot_config(const std::string &name) const;

private:
    std::string _interface_name = "uagv";
    double _update_rate_hz = 10.0;
    MqttConfig _mqtt;
    YAML::Node _robots_cfg;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // CONFIG_HPP
