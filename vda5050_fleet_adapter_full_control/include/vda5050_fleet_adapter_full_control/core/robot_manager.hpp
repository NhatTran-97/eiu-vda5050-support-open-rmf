#ifndef ROBOT_MANAGER_HPP
#define ROBOT_MANAGER_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/logger.hpp>
#include <rmf_traffic/agv/Graph.hpp>

#include "vda5050_fleet_adapter_full_control/core/robot_registration.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp"

namespace vda5050_fleet_adapter_full_control::core {

// Owns the robots of one fleet and lets more of them join while the adapter runs.
class RobotManager
{
public:
    struct Entry
    {
        RobotSpec spec;
        std::shared_ptr<rmf::VdaRobotCommandHandle> command;
        std::optional<std::size_t> charger_index;
        // Identifies the latest RMF registration attempt.
        std::shared_ptr<std::atomic<int>> registration_generation;
        // Only the update thread touches this.
        std::optional<std::chrono::steady_clock::time_point> registration_started;
        // Removed at runtime: decommissioned and no longer tracked.
        std::atomic<bool> retired{false};
    };

    struct Options
    {
        double nominal_speed = 0.0;
        bool honor_waypoint_timing = false;
        bool stitch_on_replan = false;
        rmf::RoutePolicy route_policy;
    };

    // The connector, graph and clock must outlive the manager.
    RobotManager(const rclcpp::Logger &logger, rmf::Connector &connector, std::shared_ptr<const rmf_traffic::agv::Graph> graph,
                 rclcpp::Clock::SharedPtr clock, const Options &options);

    // Register a checked robot with the connector and create its command handle.
    std::shared_ptr<Entry> add(const RobotSpec &spec);

    // Decommission a robot added at runtime and stop tracking it; false when it cannot be removed.
    bool retire(const std::string &name, std::string *error);

    // The removed robot that `spec` describes again, or null when there is none.
    std::shared_ptr<Entry> find_removed(const RobotSpec &spec) const;

    // Puts a removed robot back under the adapter's management.
    void reinstate(const std::shared_ptr<Entry> &entry);

    // The robots to update this tick.
    std::vector<std::shared_ptr<Entry>> snapshot() const;
    std::shared_ptr<Entry> find(const std::string &name) const;

private:
    rclcpp::Logger _logger;
    rmf::Connector &_connector;
    std::shared_ptr<const rmf_traffic::agv::Graph> _graph;
    rclcpp::Clock::SharedPtr _clock;
    Options _options;

    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<Entry>> _entries;
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // ROBOT_MANAGER_HPP
