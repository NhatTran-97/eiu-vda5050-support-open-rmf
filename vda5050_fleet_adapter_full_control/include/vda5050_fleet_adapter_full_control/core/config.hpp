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

// Parse VDA5050 settings, defaulting optional fields and rejecting invalid values.
class Config
{
public:
    explicit Config(const std::string &config_file);

    const std::string &interface_name() const { return _interface_name; }
    double update_rate_hz() const { return _update_rate_hz; }
    const MqttConfig &mqtt() const { return _mqtt; }
    bool honor_waypoint_timing() const { return _honor_waypoint_timing; }
    bool stitch_on_replan() const { return _stitch_on_replan; }
    bool strict_validation() const { return _strict_validation; }
    const std::optional<std::string> &server_uri() const { return _server_uri; }
    RobotConfig robot_config(const std::string &name) const;

private:
    std::string _interface_name = "uagv";
    double _update_rate_hz = 10.0;
    bool _honor_waypoint_timing = false;
    bool _stitch_on_replan = false;
    bool _strict_validation = true;
    std::optional<std::string> _server_uri;
    MqttConfig _mqtt;
    YAML::Node _robots_cfg;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // CONFIG_HPP
