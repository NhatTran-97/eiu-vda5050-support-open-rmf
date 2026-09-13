#include "vda5050_fleet_adapter_full_control/core/operator_interface.hpp"

#include <chrono>
#include <cmath>
#include <utility>

#include <rclcpp/logging.hpp>

#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

namespace vda5050_fleet_adapter_full_control::core {

namespace {

// Extract yaw without introducing a tf2 dependency.
double yaw_of(const geometry_msgs::msg::Quaternion &q)
{
    return std::atan2(2.0 * (q.w * q.z + q.x * q.y),
                      1.0 - 2.0 * (q.y * q.y + q.z * q.z));
}

// Prefix for per-robot speed-limit parameters.
constexpr const char *kSpeedLimitPrefix = "speed_limit.";

// A zero parameter value disables the speed cap.
constexpr double kNoSpeedLimit = 0.0;

// How long to wait for an AGV's verdict on an initPosition before giving up.
constexpr std::chrono::seconds kInitActionTimeout{10};

}  // namespace

std::string OperatorInterface::speed_limit_parameter(const std::string &robot_name)
{
    return std::string(kSpeedLimitPrefix) + robot_name;
}

std::string OperatorInterface::robot_of_speed_limit_parameter(const std::string &parameter)
{
    const std::string prefix = kSpeedLimitPrefix;
    if (parameter.rfind(prefix, 0) != 0)
    {
        return {};
    }
    return parameter.substr(prefix.size());
}

OperatorInterface::OperatorInterface(rclcpp::Node &node, rmf::Connector &connector, std::map<std::string, RobotHooks> hooks) 
                                                            : _node(node), _connector(connector), _hooks(std::move(hooks))
{
    for (const auto &[name, robot_hooks] : _hooks)
    {
        _init_position_result_pubs[name] = _node.create_publisher<std_msgs::msg::String>("~/" + name + "/init_position_result", rclcpp::QoS(1));

        _init_position_subs.push_back(
            _node.create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>("~/" + name + "/init_position", rclcpp::QoS(1),[this, name](
                    const geometry_msgs::msg::PoseWithCovarianceStamped::SharedPtr msg)
                {
                    on_init_position(name, *msg);
                }));

        const auto make_service = [&](const std::string &verb, std::function<std::string()> action)
        {
            return _node.create_service<std_srvs::srv::Trigger>("~/" + name + "/" + verb, [this, name, verb, action](
                    const std::shared_ptr<std_srvs::srv::Trigger::Request>,
                    std::shared_ptr<std_srvs::srv::Trigger::Response> response)
                {
                    if (!action)
                    {
                        response->success = false;
                        response->message = "not available for this robot";
                        return;
                    }
                    const std::string error = action();
                    response->success = error.empty();
                    response->message = error.empty() ? verb + "d" : error;
                    if (!response->success)
                    {
                        RCLCPP_WARN(_node.get_logger(), "%s for '%s' refused: %s", verb.c_str(), name.c_str(), error.c_str());
                    }
                });
        };

        _services.push_back(make_service("pause", robot_hooks.pause));
        _services.push_back(make_service("resume", robot_hooks.resume));

        // Apply an initial value supplied through ROS parameters.
        const double initial = _node.declare_parameter<double>(speed_limit_parameter(name), kNoSpeedLimit);
        if (initial != kNoSpeedLimit)
        {
            apply_speed_limit(name, initial);
        }

        RCLCPP_INFO(_node.get_logger(),
                    "Operator interface for '%s': %s/%s/{init_position, pause, resume}, " "parameter %s",
                    name.c_str(), _node.get_name(), name.c_str(), speed_limit_parameter(name).c_str());
    }

    _init_action_timer = _node.create_wall_timer(
        std::chrono::milliseconds(500), [this]() { poll_pending_init_actions(); });

    _on_set_params = _node.add_on_set_parameters_callback([this](const std::vector<rclcpp::Parameter> &parameters)
        {
            return on_set_parameters(parameters);
        });
    _post_set_params = _node.add_post_set_parameters_callback([this](const std::vector<rclcpp::Parameter> &parameters)
        {
            on_parameters_set(parameters);
        });
}

rcl_interfaces::msg::SetParametersResult OperatorInterface::on_set_parameters(
    const std::vector<rclcpp::Parameter> &parameters)
{
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = true;

    for (const auto &parameter : parameters)
    {
        const std::string robot = robot_of_speed_limit_parameter(parameter.get_name());
        if (robot.empty())
        {
            continue;
        }

        if (_hooks.find(robot) == _hooks.end())
        {
            result.successful = false;
            result.reason = "'" + robot + "' is not a robot in this fleet";
            return result;
        }
        if (parameter.get_type() != rclcpp::ParameterType::PARAMETER_DOUBLE)
        {
            result.successful = false;
            result.reason = parameter.get_name() + " must be a double (m/s, 0 = no cap)";
            return result;
        }

        const double limit = parameter.as_double();
        if (!std::isfinite(limit) || limit < 0.0)
        {
            result.successful = false;
            result.reason = parameter.get_name() + " must be finite and >= 0 (0 = no cap)";
            return result;
        }
    }

    return result;
}

void OperatorInterface::on_parameters_set(const std::vector<rclcpp::Parameter> &parameters)
{
    for (const auto &parameter : parameters)
    {
        const std::string robot = robot_of_speed_limit_parameter(parameter.get_name());
        // Values reaching this callback have already passed validation.
        if (robot.empty() || _hooks.find(robot) == _hooks.end())
        {
            continue;
        }
        apply_speed_limit(robot, parameter.as_double());
    }
}

void OperatorInterface::apply_speed_limit(const std::string &robot_name, double limit)
{
    const std::optional<double> cap = limit == kNoSpeedLimit ? std::nullopt : std::optional<double>(limit);

    if (!_connector.set_speed_limit(robot_name, cap))
    {
        RCLCPP_ERROR(_node.get_logger(), "speed limit for '%s' was not applied", robot_name.c_str());
    }
}

void OperatorInterface::on_init_position(
    const std::string &robot_name,
    const geometry_msgs::msg::PoseWithCovarianceStamped &msg)
{
    // Report the outcome on the companion topic; the request itself is one-way.
    const auto publish_result = [this, &robot_name](const std::string &result)
    {
        const auto it = _init_position_result_pubs.find(robot_name);
        if (it == _init_position_result_pubs.end())
        {
            return;
        }
        std_msgs::msg::String out;
        out.data = result;
        it->second->publish(out);
    };

    // Not get_data(): it withholds everything until the AGV is localized, which is exactly what this command exists to fix. Only the map name is needed.
    const auto map_name = _connector.get_known_map(robot_name);
    if (!map_name.has_value())
    {
        RCLCPP_WARN(_node.get_logger(),
                    "init_position for '%s' ignored: the AGV has never reported a VDA5050 "
                    "state, so it is not reachable yet",
                    robot_name.c_str());
        publish_result("error: no VDA5050 state from the robot yet -- is it online?");
        return;
    }

    const double x = msg.pose.pose.position.x;
    const double y = msg.pose.pose.position.y;
    const double theta = yaw_of(msg.pose.pose.orientation);

    RCLCPP_INFO(_node.get_logger(), "init_position for '%s': (%.2f, %.2f, %.2f rad) on '%s'", robot_name.c_str(), x, y, theta, map_name->c_str());

    const std::string action_id = _connector.init_position(robot_name, x, y, theta, *map_name);
    if (action_id.empty())
    {
        RCLCPP_ERROR(_node.get_logger(), "init_position for '%s' was not published",robot_name.c_str());
        publish_result("error: could not publish initPosition (MQTT transport failure)");
        return;
    }

    // Sent is not accepted -- the AGV's verdict only arrives in its actionStates.
    std::lock_guard<std::mutex> lock(_pending_mutex);
    _pending_init_actions[robot_name] = PendingInitAction{action_id, std::chrono::steady_clock::now() + kInitActionTimeout};
}

void OperatorInterface::poll_pending_init_actions()
{
    std::map<std::string, std::string> finished;
    {
        std::lock_guard<std::mutex> lock(_pending_mutex);
        for (auto it = _pending_init_actions.begin(); it != _pending_init_actions.end();)
        {
            const auto result = _connector.get_action_result(it->first, it->second.action_id);
            if (result.has_value() && vda5050::is_terminal_action_status(result->first))
            {
                finished[it->first] = result->first == "FINISHED"? "ok" : "error: robot refused -- " + (result->second.empty() ? std::string("no reason given") : result->second);
                it = _pending_init_actions.erase(it);
                continue;
            }
            if (std::chrono::steady_clock::now() >= it->second.deadline)
            {
                finished[it->first] = "error: no reply from the robot within " + std::to_string(kInitActionTimeout.count()) + "s";
                it = _pending_init_actions.erase(it);
                continue;
            }
            ++it;
        }
    }

    for (const auto &[robot_name, result] : finished)
    {
        const auto it = _init_position_result_pubs.find(robot_name);
        if (it == _init_position_result_pubs.end())
        {
            continue;
        }
        std_msgs::msg::String out;
        out.data = result;
        it->second->publish(out);
        RCLCPP_INFO(_node.get_logger(), "init_position for '%s': %s", robot_name.c_str(), result.c_str());
    }
}

}  // namespace vda5050_fleet_adapter_full_control::core
