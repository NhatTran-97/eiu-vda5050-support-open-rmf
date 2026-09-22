#ifndef REGISTRATION_INTERFACE_HPP
#define REGISTRATION_INTERFACE_HPP

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rclcpp/node.hpp>
#include <std_msgs/msg/string.hpp>

#include "vda5050_fleet_adapter_full_control/core/operator_interface.hpp"
#include "vda5050_fleet_adapter_full_control/core/robot_manager.hpp"
#include "vda5050_fleet_adapter_full_control/core/robot_registration.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

namespace vda5050_fleet_adapter_full_control::core {

// Names of the ROS topics that carry robot registration as JSON strings.
inline constexpr const char *kRegistrationRequestTopic = "/robot_registration_requests";
inline constexpr const char *kRegistrationResultTopic = "/robot_registration_results";
inline constexpr const char *kRobotRegistryTopic = "/robot_registry";
inline constexpr const char *kRobotDiscoveryTopic = "/robot_discovery";

// Adds and removes robots at runtime, reports unregistered robots on the broker and publishes each fleet's robots.
class RegistrationInterface
{
public:
    struct Config
    {
        std::string fleet_name;
        std::string interface_name;
        // Where robots added at runtime are kept for the next start.
        std::string runtime_path;
        FleetLimits limits;
        GraphFacts graph;
        // Whether a robot added without saying so waits in place responsively; the fleet's own default.
        bool default_responsive_wait = false;
        // Seconds after startup before unknown robots are reported.
        double discovery_grace_s = 0.0;
        // Seconds between checks of the broker for unknown robots.
        double discovery_period_s = 1.0;
    };

    // Everything passed by reference must outlive this interface.
    RegistrationInterface(rclcpp::Node &node, rmf::Connector &connector, RobotManager &manager,
                          OperatorInterface &operator_interface, Config config);

    // Publish this fleet's robots, chargers, type and limits when they changed.
    void publish_registry();

    // Handles a JSON registration request; null when it is for another fleet. A repeated request_id gets the earlier result.
    nlohmann::json handle_request(const nlohmann::json &request);

    // Learn the robots of another fleet from its registry message.
    void on_registry_message(const nlohmann::json &registry);

    // Publish the robots that are online on the broker but registered nowhere, when that list changes.
    void poll_discovery();

private:
    // The handlers below run with _mutex held.
    nlohmann::json handle_add(const nlohmann::json &request);
    nlohmann::json handle_remove(const nlohmann::json &request);

    FleetView fleet_view() const;
    std::vector<KnownRobot> own_robots() const;
    nlohmann::json registry_json() const;
    // Publish the registry if it differs from the last one; the caller holds _mutex.
    void publish_registry_locked();
    bool save_runtime(std::string *error);

    static void publish(const rclcpp::Publisher<std_msgs::msg::String>::SharedPtr &pub, const nlohmann::json &message);

    rclcpp::Node &_node;
    rmf::Connector &_connector;
    RobotManager &_manager;
    OperatorInterface &_operator_interface;
    Config _config;
    std::chrono::steady_clock::time_point _started;

    // Serializes requests, registry updates and discovery reports.
    mutable std::mutex _mutex;
    // Robots of the other fleets, keyed by fleet name.
    std::map<std::string, std::vector<KnownRobot>> _other_fleets;
    // Results of the latest requests by request_id, so a resent request is answered, not run twice.
    std::map<std::string, nlohmann::json> _answered;
    std::deque<std::string> _answered_order;
    // The runtime robots file has been copied to .bak in this run.
    bool _backed_up = false;
    // The registry last published, as text.
    std::string _last_registry;
    // Signature of the pending-robot list last published, so only changes are sent.
    std::optional<std::string> _discovery_signature;
    // "manufacturer/serial" of pending robots already logged.
    std::set<std::string> _announced;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr _result_pub;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr _registry_pub;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr _discovery_pub;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr _request_sub;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr _registry_sub;
    rclcpp::TimerBase::SharedPtr _discovery_timer;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // REGISTRATION_INTERFACE_HPP
