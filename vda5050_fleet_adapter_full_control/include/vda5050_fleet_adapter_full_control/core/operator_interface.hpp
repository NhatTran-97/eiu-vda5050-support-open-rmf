#ifndef OPERATOR_INTERFACE_HPP
#define OPERATOR_INTERFACE_HPP

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/node_interfaces/node_parameters_interface.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

namespace vda5050_fleet_adapter_full_control::core {

// Operator commands provided by one robot command handle. Each callback returns an empty string on success or an error description on failure.
struct RobotHooks
{
    std::function<std::string()> pause;
    std::function<std::string()> resume;
};

// Exposes operator controls that are not part of RobotCommandHandle.
//
// Per robot <name>:
//   <node>/<name>/init_position         (geometry_msgs/PoseWithCovarianceStamped)
//   <node>/<name>/init_position_result  (std_msgs/String, published back: "ok" or "error: <reason>")
//   <node>/<name>/pause                 (std_srvs/Trigger)
//   <node>/<name>/resume                (std_srvs/Trigger)
//
// Per-robot speed cap:
//   speed_limit.<name>  (double, m/s; 0.0 disables the cap)
class OperatorInterface
{
public:
    // `node` and `connector` must outlive this object.
    OperatorInterface(rclcpp::Node &node, rmf::Connector &connector, std::map<std::string, RobotHooks> hooks);

private:
    // An initPosition sent to an AGV, awaiting its verdict in actionStates.
    struct PendingInitAction
    {
        std::string action_id;
        std::chrono::steady_clock::time_point deadline;
    };

    void on_init_position(const std::string &robot_name, const geometry_msgs::msg::PoseWithCovarianceStamped &msg);

    // Publish the AGV's verdict on each pending initPosition once it lands or times out.
    void poll_pending_init_actions();

    // Validate speed-limit updates before ROS commits them.
    rcl_interfaces::msg::SetParametersResult on_set_parameters(const std::vector<rclcpp::Parameter> &parameters);

    // Apply speed-limit updates after ROS commits them.
    void on_parameters_set(const std::vector<rclcpp::Parameter> &parameters);

    // Forward a validated speed limit to the connector.
    void apply_speed_limit(const std::string &robot_name, double limit);

    // Convert between robot names and speed-limit parameter names.
    static std::string speed_limit_parameter(const std::string &robot_name);
    static std::string robot_of_speed_limit_parameter(const std::string &parameter);

    rclcpp::Node &_node;
    rmf::Connector &_connector;
    std::map<std::string, RobotHooks> _hooks;

    std::map<std::string, rclcpp::Publisher<std_msgs::msg::String>::SharedPtr> _init_position_result_pubs;
    std::mutex _pending_mutex;
    std::map<std::string, PendingInitAction> _pending_init_actions;

    // Declared last so they are destroyed first: their callbacks read the members above.
    std::vector<rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr> _init_position_subs;
    std::vector<rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr> _services;
    rclcpp::TimerBase::SharedPtr _init_action_timer;

    // Held only to keep the registrations alive for this object's lifetime;
    // dropping either shared_ptr deregisters its callback. Not read again after construction.
    rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr _on_set_params;
    rclcpp::node_interfaces::PostSetParametersCallbackHandle::SharedPtr _post_set_params;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // OPERATOR_INTERFACE_HPP
