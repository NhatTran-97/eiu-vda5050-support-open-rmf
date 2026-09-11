#include "vda5050_fleet_adapter_full_control/core/config.hpp"

#include <cmath>
#include <stdexcept>

namespace vda5050_fleet_adapter_full_control::core {

Args parse_args(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config_file") && i + 1 < argc)
        {
            a.config_file = argv[++i];
        }
        else if ((arg == "-n" || arg == "--nav_graph") && i + 1 < argc)
        {
            a.nav_graph = argv[++i];
        }
    }
    return a;
}

Config::Config(const std::string &config_file)
{
    const YAML::Node root = YAML::LoadFile(config_file);
    const YAML::Node vda = root["vda5050"];

    _interface_name = vda["interface_name"] ? vda["interface_name"].as<std::string>() : "uagv";
    if (_interface_name.empty())
    {
        throw std::runtime_error("vda5050.interface_name must not be empty");
    }

    // The update period requires a finite, positive frequency.
    _update_rate_hz = vda["update_rate_hz"] ? vda["update_rate_hz"].as<double>() : 10.0;
    if (!std::isfinite(_update_rate_hz) || !(_update_rate_hz > 0.0))
    {
        throw std::runtime_error("vda5050.update_rate_hz must be a finite value > 0");
    }

    const YAML::Node mqtt = vda["mqtt"];
    const std::string host = (mqtt && mqtt["host"]) ? mqtt["host"].as<std::string>() : "localhost";
    if (host.empty())
    {
        throw std::runtime_error("vda5050.mqtt.host must not be empty");
    }
    const int port = (mqtt && mqtt["port"]) ? mqtt["port"].as<int>() : 1883;
    if (port < 1 || port > 65535)
    {
        throw std::runtime_error("vda5050.mqtt.port must be between 1 and 65535");
    }
    _mqtt.broker_url = "tcp://" + host + ":" + std::to_string(port);
    if (mqtt && mqtt["username"] && !mqtt["username"].IsNull())
    {
        _mqtt.username = mqtt["username"].as<std::string>();
    }
    if (mqtt && mqtt["password"] && !mqtt["password"].IsNull())
    {
        _mqtt.password = mqtt["password"].as<std::string>();
    }

    _robots_cfg = vda["robots"];
}

RobotConfig Config::robot_config(const std::string &name) const
{
    RobotConfig cfg;
    cfg.serial = name;

    const YAML::Node rc = _robots_cfg ? _robots_cfg[name] : YAML::Node();
    if (rc && rc["manufacturer"])
    {
        cfg.manufacturer = rc["manufacturer"].as<std::string>();
    }
    if (rc && rc["serial"])
    {
        cfg.serial = rc["serial"].as<std::string>();
    }
    if (cfg.manufacturer.empty() || cfg.serial.empty())
    {
        throw std::runtime_error("robot '" + name + "': manufacturer/serial must not be empty");
    }

    if (rc && rc["transform"])
    {
        const auto t = rc["transform"];
        const double rot = t["rotation"] ? t["rotation"].as<double>() : 0.0;
        // Transform::to_rmf() requires an invertible scale.
        const double scale = t["scale"] ? t["scale"].as<double>() : 1.0;
        if (!std::isfinite(scale) || scale == 0.0)
        {
            throw std::runtime_error(
                "robot '" + name + "': transform.scale must be finite and non-zero");
        }
        double tx = 0.0, ty = 0.0;
        if (t["translation"] && t["translation"].size() >= 2)
        {
            tx = t["translation"][0].as<double>();
            ty = t["translation"][1].as<double>();
        }
        cfg.transform = rmf::Transform(rot, scale, tx, ty);
    }

    return cfg;
}

}  // namespace vda5050_fleet_adapter_full_control::core
