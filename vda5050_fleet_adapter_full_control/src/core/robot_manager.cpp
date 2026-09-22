#include "vda5050_fleet_adapter_full_control/core/robot_manager.hpp"

#include <algorithm>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter_full_control::core {

RobotManager::RobotManager(const rclcpp::Logger &logger, rmf::Connector &connector,
                           std::shared_ptr<const rmf_traffic::agv::Graph> graph, rclcpp::Clock::SharedPtr clock,
                           const Options &options)
    : _logger(logger),
      _connector(connector),
      _graph(std::move(graph)),
      _clock(std::move(clock)),
      _options(options)
{
}

std::shared_ptr<RobotManager::Entry> RobotManager::add(const RobotSpec &spec)
{
    auto entry = std::make_shared<Entry>();
    entry->spec = spec;
    entry->registration_generation = std::make_shared<std::atomic<int>>(0);

    if (!spec.charger.empty())
    {
        if (const auto *charger = _graph->find_waypoint(spec.charger))
        {
            entry->charger_index = charger->index();
        }
        else
        {
            RCLCPP_ERROR(_logger,
                         "Robot '%s': charger waypoint '%s' not found in the nav graph -- it will use whatever "
                         "charger RMF finds nearest",
                         spec.name.c_str(), spec.charger.c_str());
        }
    }

    _connector.add_robot(spec.name, spec.manufacturer, spec.serial,
                         rmf::Transform(spec.rotation, spec.scale, spec.tx, spec.ty));
    entry->command = std::make_shared<rmf::VdaRobotCommandHandle>(
        _logger, spec.name, _connector, _graph, _options.nominal_speed, _clock, _options.honor_waypoint_timing,
        _options.stitch_on_replan, _options.route_policy);

    std::lock_guard<std::mutex> lock(_mutex);
    _entries.push_back(entry);
    return entry;
}

bool RobotManager::retire(const std::string &name, std::string *error)
{
    const auto entry = find(name);
    if (!entry)
    {
        if (error)
        {
            *error = "no robot named '" + name + "' in this fleet";
        }
        return false;
    }
    if (entry->spec.from_config)
    {
        if (error)
        {
            *error = "'" + name + "' is defined in the fleet config file; remove it there and restart the adapter";
        }
        return false;
    }
    if (entry->retired.exchange(true))
    {
        if (error)
        {
            *error = "'" + name + "' was already removed";
        }
        return false;
    }

    // RMF has no call to drop a robot, so it stays decommissioned until the adapter restarts.
    entry->command->set_ready_for_orders(false, "removed by the operator");
    // Nothing manages the robot from here on, so it must not carry on with an order.
    if (_connector.stop(name) == rmf::CommandStatus::transport_failed)
    {
        RCLCPP_WARN(_logger, "Robot '%s': cancelOrder did not reach the AGV; it may finish its last order", name.c_str());
    }
    RCLCPP_WARN(_logger, "Robot '%s' removed: decommissioned and no longer tracked", name.c_str());
    return true;
}

std::shared_ptr<RobotManager::Entry> RobotManager::find_removed(const RobotSpec &spec) const
{
    const auto entry = find(spec.name);
    return entry && entry->retired.load() && same_robot(entry->spec, spec) ? entry : nullptr;
}

void RobotManager::reinstate(const std::shared_ptr<Entry> &entry)
{
    entry->retired = false;
    RCLCPP_INFO(_logger, "Robot '%s' restored to the fleet", entry->spec.name.c_str());
}

std::vector<std::shared_ptr<RobotManager::Entry>> RobotManager::snapshot() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _entries;
}

std::shared_ptr<RobotManager::Entry> RobotManager::find(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = std::find_if(_entries.begin(), _entries.end(),
                                 [&](const std::shared_ptr<Entry> &e) { return e->spec.name == name; });
    return it == _entries.end() ? nullptr : *it;
}

}  // namespace vda5050_fleet_adapter_full_control::core
