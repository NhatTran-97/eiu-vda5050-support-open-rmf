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

// Parses the `vda5050` configuration block. Invalid YAML, invalid field
// types, and unsupported values are reported as exceptions. Optional fields
// use the defaults declared by this class.
class Config
{
public:
    explicit Config(const std::string &config_file);

    const std::string &interface_name() const { return _interface_name; }
    double update_rate_hz() const { return _update_rate_hz; }
    const MqttConfig &mqtt() const { return _mqtt; }
    // When true, an order releases only as far as Plan::Waypoint::time()
    // allows, growing via VDA5050 order updates as those times pass --
    // the AGV waits at the release boundary instead of outrunning RMF's
    // schedule. Off by default: a lone robot has nothing to protect
    // against. Also permanently disables RobotUpdateHandle::maximum_delay()
    // (see set_update_handle()).
    bool honor_waypoint_timing() const { return _honor_waypoint_timing; }

    // Missing optional values use the default manufacturer, the robot name as
    // serial, and an identity transform.
    RobotConfig robot_config(const std::string &name) const;

private:
    std::string _interface_name = "uagv";
    double _update_rate_hz = 10.0;
    bool _honor_waypoint_timing = false;
    MqttConfig _mqtt;
    YAML::Node _robots_cfg;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // CONFIG_HPP
