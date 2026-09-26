#include "vda5050_fleet_adapter_full_control/core/config.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace vda5050_fleet_adapter_full_control::core {

namespace {

// The string value of an optional key of `map`, or empty when the key is absent or null.
std::string optional_string(const YAML::Node &map, const char *key, const std::string &path)
{
    if (!map || !map[key] || map[key].IsNull())
    {
        return {};
    }
    if (!map[key].IsScalar())
    {
        throw std::runtime_error(path + "." + key + " must be a string");
    }
    return map[key].as<std::string>();
}

// Optional whole number of `map` within [low, high]; nullopt when absent or null.
std::optional<int> optional_whole(const YAML::Node &map, const char *key, const std::string &path, int low, int high)
{
    if (!map || !map[key] || map[key].IsNull())
    {
        return std::nullopt;
    }
    const std::string requirement = path + "." + key + " must be an integer in [" + std::to_string(low) + ", " + std::to_string(high) + "]";
    int value = 0;
    try
    {
        value = map[key].as<int>();
    }
    catch (const YAML::Exception &)
    {
        throw std::runtime_error(requirement);
    }
    if (value < low || value > high)
    {
        throw std::runtime_error(requirement);
    }
    return value;
}

// The number `value` as short text, for messages.
std::string bound_text(double value)
{
    std::ostringstream text;
    text << value;
    return text.str();
}

// Optional finite number of `map` within [low, high]; nullopt when absent or null.
std::optional<double> optional_number(const YAML::Node &map, const char *key, const std::string &path, double low, double high)
{
    if (!map || !map[key] || map[key].IsNull())
    {
        return std::nullopt;
    }
    const std::string requirement = path + "." + key + " must be a number in [" + bound_text(low) + ", " + bound_text(high) + "]";
    double value = 0.0;
    try
    {
        value = map[key].as<double>();
    }
    catch (const YAML::Exception &)
    {
        throw std::runtime_error(requirement);
    }
    if (!std::isfinite(value) || value < low || value > high)
    {
        throw std::runtime_error(requirement);
    }
    return value;
}

// The boolean value of an optional key of `map`, or `fallback` when the key is absent or null.
bool optional_bool(const YAML::Node &map, const char *key, const std::string &path, bool fallback)
{
    if (!map || !map[key] || map[key].IsNull())
    {
        return fallback;
    }
    try
    {
        return map[key].as<bool>();
    }
    catch (const YAML::Exception &)
    {
        throw std::runtime_error(path + "." + key + " must be true or false");
    }
}

// Replace each ${NAME} in `text` with the value of that environment variable.
std::string expand_environment(const std::string &text, const std::string &path)
{
    std::string expanded;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const auto open = text.find("${", pos);
        if (open == std::string::npos)
        {
            expanded += text.substr(pos);
            break;
        }
        const auto close = text.find('}', open + 2);
        if (close == std::string::npos)
        {
            throw std::runtime_error(path + " has a '${' without a closing '}'");
        }
        expanded += text.substr(pos, open - pos);
        const std::string name = text.substr(open + 2, close - open - 2);
        // The environment is read while the configuration loads, before any thread exists.
        const char *value = name.empty() ? nullptr : std::getenv(name.c_str());  // NOLINT(concurrency-mt-unsafe)
        if (!value)
        {
            throw std::runtime_error(path + " uses ${" + name + "}, which is not set in the environment");
        }
        expanded += value;
        pos = close + 1;
    }
    return expanded;
}

// YAML scalar to JSON bool, integer, number or string.
nlohmann::json scalar_json(const YAML::Node &node)
{
    const std::string text = node.as<std::string>();
    if (text == "true" || text == "false")
    {
        return text == "true";
    }
    try
    {
        std::size_t used = 0;
        const long long whole = std::stoll(text, &used);
        if (used == text.size())
        {
            return whole;
        }
        const double number = std::stod(text, &used);
        if (used == text.size())
        {
            return number;
        }
    }
    catch (const std::exception &)
    {
    }
    return text;
}

// Parse vda5050.dock_actions.
std::map<std::string, rmf::DockAction> read_dock_actions(const YAML::Node &node)
{
    std::map<std::string, rmf::DockAction> actions;
    if (!node || node.IsNull())
    {
        return actions;
    }
    if (!node.IsMap())
    {
        throw std::runtime_error("vda5050.dock_actions must be a map of dock name to {action, parameters}");
    }
    for (const auto &entry : node)
    {
        const std::string dock = entry.first.as<std::string>();
        const std::string path = "vda5050.dock_actions." + dock;
        const YAML::Node &spec = entry.second;
        rmf::DockAction action;
        action.action_type = spec.IsMap() ? optional_string(spec, "action", path) : std::string{};
        if (action.action_type.empty())
        {
            throw std::runtime_error(path + ".action must name a VDA5050 action type");
        }
        const YAML::Node parameters = spec["parameters"];
        if (parameters && !parameters.IsNull())
        {
            if (!parameters.IsMap())
            {
                throw std::runtime_error(path + ".parameters must be a map of key to value");
            }
            for (const auto &parameter : parameters)
            {
                if (!parameter.second.IsScalar())
                {
                    throw std::runtime_error(path + ".parameters." + parameter.first.as<std::string>() + " must be a single value");
                }
                action.parameters[parameter.first.as<std::string>()] = scalar_json(parameter.second);
            }
        }
        actions[dock] = std::move(action);
    }
    return actions;
}

}  // namespace

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

    // Limit the update rate so whole-millisecond timer intervals remain valid.
    _update_rate_hz = vda["update_rate_hz"] ? vda["update_rate_hz"].as<double>() : 10.0;
    if (!std::isfinite(_update_rate_hz) || !(_update_rate_hz > 0.0) || _update_rate_hz > 100.0)
    {
        throw std::runtime_error("vda5050.update_rate_hz must be a finite value in (0, 100]");
    }

    _honor_waypoint_timing = vda["honor_waypoint_timing"] ? vda["honor_waypoint_timing"].as<bool>() : false;
    _stitch_on_replan = vda["stitch_on_replan"] ? vda["stitch_on_replan"].as<bool>() : false;
    _strict_validation = vda["strict_validation"] ? vda["strict_validation"].as<bool>() : true;

    if (const auto streak = optional_whole(vda, "stale_state_streak", "vda5050", 0, 100))
    {
        _stale_state_streak = *streak;
    }
    if (const auto seconds = optional_whole(vda, "cancel_confirm_timeout_s", "vda5050", 0, 120))
    {
        _cancel_policy.confirm_timeout = std::chrono::seconds(*seconds);
    }
    if (const auto attempts = optional_whole(vda, "cancel_attempts", "vda5050", 1, 10))
    {
        _cancel_policy.attempts = *attempts;
    }

    // Read one optional number of the vda5050 section into `value`.
    const auto read_number = [&vda](const char *key, double &value, double low, double high)
    {
        if (const auto number = optional_number(vda, key, "vda5050", low, high))
        {
            value = *number;
        }
    };
    read_number("state_timeout_s", _link_policy.state_timeout_s, 1.0, 3600.0);
    read_number("offline_state_intervals", _link_policy.offline_state_intervals, 1.0, 100.0);
    read_number("order_stuck_timeout_s", _link_policy.order_stuck_timeout_s, 1.0, 3600.0);
    read_number("order_ack_timeout_s", _link_policy.order_ack_timeout_s, 0.1, 3600.0);
    if (const auto attempts = optional_whole(vda, "order_resend_attempts", "vda5050", 0, 10))
    {
        _link_policy.order_resend_attempts = *attempts;
    }
    _link_policy.cancel_unknown_orders = optional_bool(vda, "cancel_unknown_orders", "vda5050", true);
    _route_policy.replan_after_s = _link_policy.order_stuck_timeout_s;
    read_number("factsheet_first_wait_s", _link_policy.factsheet_first_wait_s, 0.0, 3600.0);
    read_number("factsheet_retry_wait_s", _link_policy.factsheet_retry_wait_s, 1.0, 3600.0);
    if (const auto attempts = optional_whole(vda, "factsheet_request_attempts", "vda5050", 0, 10))
    {
        _link_policy.factsheet_request_attempts = *attempts;
    }
    read_number("waypoint_reached_m", _route_policy.waypoint_reached_m, 0.01, 10.0);
    read_number("same_pose_m", _route_policy.same_pose_m, 0.001, 1.0);
    read_number("same_pose_rad", _route_policy.same_pose_rad, 0.001, 1.0);
    read_number("usable_speed_mps", _route_policy.usable_speed_mps, 0.0, 1.0);
    read_number("early_arrival_warn_s", _route_policy.early_arrival_warn_s, 0.0, 3600.0);
    read_number("traffic_pause_timeout_s", _route_policy.traffic_pause_timeout_s, 0.0, 3600.0);
    read_number("timed_release_max_delay_s", _route_policy.timed_release_max_delay_s, 0.0, 3600.0);
    read_number("node_deviation_xy_m", _node_deviation.xy_m, 0.01, 100.0);
    read_number("node_deviation_theta_rad", _node_deviation.theta_rad, 0.001, 3.1416);
    read_number("init_position_timeout_s", _init_position_timeout_s, 1.0, 600.0);
    _route_policy.cap_speed_to_fleet = optional_bool(vda, "cap_edge_speed_to_fleet", "vda5050", false);
    _action_policy.dock_actions = read_dock_actions(vda["dock_actions"]);
    _action_policy.charge_at_chargers = optional_bool(vda, "charge_at_chargers", "vda5050", false);
    read_number("metrics_period_s", _metrics_period_s, 0.0, 3600.0);

    const YAML::Node registration_node = vda["registration"];
    if (registration_node && !registration_node.IsNull())
    {
        if (!registration_node.IsMap())
        {
            throw std::runtime_error("vda5050.registration must be a map");
        }
        // Read one optional number and require it to lie in [low, high].
        const auto read_range = [&registration_node](const char *key, double &value, double low, double high)
        {
            if (const auto number = optional_number(registration_node, key, "vda5050.registration", low, high))
            {
                value = *number;
            }
        };
        read_range("discovery_grace_s", _registration.discovery_grace_s, 0.0, 3600.0);
        read_range("discovery_period_s", _registration.discovery_period_s, 0.1, 3600.0);
        read_range("timeout_s", _registration.timeout_s, 1.0, 600.0);
        read_range("limit_tolerance", _registration.limit_tolerance, 0.0, 0.5);
        if (registration_node["runtime_robots_file"] && !registration_node["runtime_robots_file"].IsNull())
        {
            _registration.runtime_robots_file = registration_node["runtime_robots_file"].as<std::string>();
        }
    }

    if (vda["ui_websocket_uri"] && !vda["ui_websocket_uri"].IsNull())
    {
        const std::string uri = vda["ui_websocket_uri"].as<std::string>();
        if (!uri.empty())
        {
            _server_uri = uri;
        }
    }

    const YAML::Node mqtt_node = vda["mqtt"];
    const std::string host = (mqtt_node && mqtt_node["host"]) ? mqtt_node["host"].as<std::string>() : "localhost";
    if (host.empty())
    {
        throw std::runtime_error("vda5050.mqtt.host must not be empty");
    }

    const YAML::Node tls = mqtt_node ? mqtt_node["tls"] : YAML::Node();
    if (tls && !tls.IsNull())
    {
        if (!tls.IsMap())
        {
            throw std::runtime_error("vda5050.mqtt.tls must be a map");
        }
        _mqtt.options.tls.enabled = optional_bool(tls, "enabled", "vda5050.mqtt.tls", false);
        if (_mqtt.options.tls.enabled)
        {
            const std::filesystem::path base = std::filesystem::path(config_file).parent_path();
            // Path of a readable file named by a tls key, relative to the config file; empty when the key is unset.
            const auto readable_file = [&](const char *key) -> std::string
            {
                const std::string name = optional_string(tls, key, "vda5050.mqtt.tls");
                if (name.empty())
                {
                    return {};
                }
                std::string full = (base / name).string();
                std::error_code error;
                if (!std::filesystem::is_regular_file(full, error) || !std::ifstream(full))
                {
                    throw std::runtime_error(std::string("vda5050.mqtt.tls.") + key + " '" + full + "' is not a readable file");
                }
                return full;
            };
            _mqtt.options.tls.ca_file = readable_file("ca_file");
            _mqtt.options.tls.client_cert = readable_file("client_cert");
            _mqtt.options.tls.client_key = readable_file("client_key");
            if (_mqtt.options.tls.client_cert.empty() != _mqtt.options.tls.client_key.empty())
            {
                throw std::runtime_error("vda5050.mqtt.tls.client_cert and client_key must be set together");
            }
            _mqtt.options.tls.verify_hostname = optional_bool(tls, "verify_hostname", "vda5050.mqtt.tls", true);
        }
    }

    const int port = (mqtt_node && mqtt_node["port"]) ? mqtt_node["port"].as<int>() : (_mqtt.options.tls.enabled ? 8883 : 1883);
    if (port < 1 || port > 65535)
    {
        throw std::runtime_error("vda5050.mqtt.port must be between 1 and 65535");
    }
    _mqtt.broker_url = std::string(_mqtt.options.tls.enabled ? "ssl://" : "tcp://") + host + ":" + std::to_string(port);
    const auto read_whole = [&mqtt_node](const char *key, int low, int high)
    {
        return optional_whole(mqtt_node, key, "vda5050.mqtt", low, high);
    };
    if (const auto value = read_whole("connect_timeout_s", 1, 120))
    {
        _mqtt.options.connect_timeout = std::chrono::seconds(*value);
    }
    if (const auto value = read_whole("keep_alive_s", 1, 3600))
    {
        _mqtt.options.keep_alive = std::chrono::seconds(*value);
    }
    if (const auto value = read_whole("reconnect_min_s", 1, 60))
    {
        _mqtt.options.retry_min = std::chrono::seconds(*value);
    }
    if (const auto value = read_whole("reconnect_max_s", 1, 600))
    {
        _mqtt.options.retry_max = std::chrono::seconds(*value);
    }
    if (const auto value = read_whole("max_payload_bytes", 1024, 268435456))
    {
        _mqtt.options.max_payload_bytes = static_cast<std::size_t>(*value);
    }
    if (const auto value = read_whole("qos", 0, 2))
    {
        _mqtt.options.qos = *value;
    }
    if (_mqtt.options.retry_max < _mqtt.options.retry_min)
    {
        throw std::runtime_error("vda5050.mqtt.reconnect_max_s must not be below reconnect_min_s");
    }
    if (mqtt_node && mqtt_node["username"] && !mqtt_node["username"].IsNull())
    {
        _mqtt.username = expand_environment(mqtt_node["username"].as<std::string>(), "vda5050.mqtt.username");
    }
    if (mqtt_node && mqtt_node["password"] && !mqtt_node["password"].IsNull())
    {
        _mqtt.password = expand_environment(mqtt_node["password"].as<std::string>(), "vda5050.mqtt.password");
    }

    // Leave the robots unset when the key is absent, so robot_config() can name the missing robot.
    if (vda["robots"])
    {
        _robots_cfg = vda["robots"];
    }
}

RobotConfig Config::robot_config(const std::string &name) const
{
    RobotConfig cfg;
    cfg.serial = name;

    const YAML::Node rc = _robots_cfg ? _robots_cfg[name] : YAML::Node();
    if (!rc)
    {
        throw std::runtime_error( "robot '" + name + "' is in the fleet's nav graph but has no entry " "under vda5050.robots in config.yaml");
    }
    if (rc["manufacturer"])
    {
        cfg.manufacturer = rc["manufacturer"].as<std::string>();
    }
    if (rc["serial"])
    {
        cfg.serial = rc["serial"].as<std::string>();
    }
    if (cfg.manufacturer.empty() || cfg.serial.empty())
    {
        throw std::runtime_error("robot '" + name + "': manufacturer/serial must not be empty");
    }

    if (rc["transform"])
    {
        const auto t = rc["transform"];
        const double rot = t["rotation"] ? t["rotation"].as<double>() : 0.0;
        // Require a nonzero scale for the inverse map transform.
        const double scale = t["scale"] ? t["scale"].as<double>() : 1.0;
        if (!std::isfinite(scale) || scale == 0.0)
        {
            throw std::runtime_error("robot '" + name + "': transform.scale must be finite and non-zero");
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
