#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <optional>
#include <string>

#include <yaml-cpp/yaml.h>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_options.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/action_policy.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/link_policy.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/route_policy.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/cancel_policy.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_sequence.hpp"

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
    mqtt::MqttOptions options;
};

struct RobotConfig
{
    std::string manufacturer = "unknown";
    std::string serial;
    rmf::Transform transform;
};

// Settings for robots that are added to a running adapter.
struct RegistrationConfig
{
    // Seconds after startup before robots unknown to every fleet are reported.
    double discovery_grace_s = 8.0;
    // Seconds between checks of the broker for such robots.
    double discovery_period_s = 2.0;
    // Seconds RMF may take to complete a robot's registration before it is tried again.
    double timeout_s = 30.0;
    // Relative tolerance when comparing a robot's speed and acceleration with the fleet's.
    double limit_tolerance = 0.05;
    // Where robots added at runtime are kept; empty means next to the config file.
    std::string runtime_robots_file;
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
    int stale_state_streak() const { return _stale_state_streak; }
    const vda5050::CancelPolicy &cancel_policy() const { return _cancel_policy; }
    const rmf::LinkPolicy &link_policy() const { return _link_policy; }
    const rmf::RoutePolicy &route_policy() const { return _route_policy; }
    const vda5050::NodeDeviation &node_deviation() const { return _node_deviation; }
    const rmf::ActionPolicy &action_policy() const { return _action_policy; }
    double init_position_timeout_s() const { return _init_position_timeout_s; }
    double metrics_period_s() const { return _metrics_period_s; }
    const RegistrationConfig &registration() const { return _registration; }
    const std::optional<std::string> &server_uri() const { return _server_uri; }
    RobotConfig robot_config(const std::string &name) const;

private:
    std::string _interface_name = "uagv";
    double _update_rate_hz = 10.0;
    bool _honor_waypoint_timing = false;
    bool _stitch_on_replan = false;
    bool _strict_validation = true;
    int _stale_state_streak = vda5050::StateSequence::kDefaultStreakLimit;
    vda5050::CancelPolicy _cancel_policy;
    rmf::LinkPolicy _link_policy;
    rmf::RoutePolicy _route_policy;
    vda5050::NodeDeviation _node_deviation;
    rmf::ActionPolicy _action_policy;
    double _init_position_timeout_s = 10.0;
    double _metrics_period_s = 60.0;
    RegistrationConfig _registration;
    std::optional<std::string> _server_uri;
    MqttConfig _mqtt;
    YAML::Node _robots_cfg;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // CONFIG_HPP
