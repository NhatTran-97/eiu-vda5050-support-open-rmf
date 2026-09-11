#include "vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include <rclcpp/logging.hpp>
#include <rmf_traffic_ros2/Time.hpp>

namespace vda5050_fleet_adapter_full_control::rmf {

namespace {
// Position tolerance for waypoint progress when nodeId is unavailable.
constexpr double kWaypointReachedMetres = 0.5;

// Speeds below this threshold use the configured nominal speed for estimates.
constexpr double kUsableSpeedMetresPerSecond = 0.05;

// Early-arrival threshold for schedule diagnostics.
constexpr double kEarlyArrivalWarnSeconds = 2.0;
}  // namespace

VdaRobotCommandHandle::VdaRobotCommandHandle(rclcpp::Logger logger, std::string name, Connector &connector,
                                                std::shared_ptr<const rmf_traffic::agv::Graph> graph, double nominal_speed,
                                                rclcpp::Clock::SharedPtr clock) : _logger(std::move(logger)), _name(std::move(name)), _connector(connector),
                                                                                    _graph(std::move(graph)),
                                                                                    _nominal_speed(nominal_speed > 0.0 ? nominal_speed : 0.5),
                                                                                    _clock(std::move(clock))
{
}

std::string VdaRobotCommandHandle::derive_node_id(const std::string &name,
                                                  std::optional<std::size_t> graph_index,
                                                  double x, double y)
{
    if (!name.empty())
    {
        return name;
    }
    if (graph_index.has_value())
    {
        return "wp_" + std::to_string(*graph_index);
    }

    const auto snap_zero = [](double v) { return std::fabs(v) < 0.005 ? 0.0 : v; };

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f_%.2f", snap_zero(x), snap_zero(y));
    return buf;
}

std::string VdaRobotCommandHandle::node_id_for(
    const rmf_traffic::agv::Plan::Waypoint &wp) const
{
    const Eigen::Vector3d p = wp.position();
    std::string name;
    if (_graph && wp.graph_index().has_value())
    {
        // Graph::Waypoint::name() returns nullptr for an unnamed waypoint.
        const auto *n = _graph->get_waypoint(*wp.graph_index()).name();
        if (n)
        {
            name = *n;
        }
    }
    return derive_node_id(name, wp.graph_index(), p.x(), p.y());
}

std::optional<double> VdaRobotCommandHandle::lane_speed_limit(
    const rmf_traffic::agv::Plan::Waypoint &wp) const
{
    if (!_graph)
    {
        return std::nullopt;
    }

    std::optional<double> limit;
    for (const std::size_t lane_index : wp.approach_lanes())
    {
        if (lane_index >= _graph->num_lanes())
        {
            continue;
        }
        const auto lane_limit = _graph->get_lane(lane_index).properties().speed_limit();
        if (!lane_limit.has_value())
        {
            continue;
        }
        limit = limit.has_value() ? std::min(*limit, *lane_limit) : *lane_limit;
    }
    return limit;
}

double VdaRobotCommandHandle::estimate_seconds(
    const Eigen::Vector3d &from, const Eigen::Vector3d &to,
    const std::optional<vda5050::Velocity> &velocity) const
{
    // Estimate straight-line travel using measured speed when available.
    double speed = _nominal_speed;
    if (velocity.has_value() && velocity->speed() >= kUsableSpeedMetresPerSecond)
    {
        speed = velocity->speed();
    }

    const double dx = to.x() - from.x();
    const double dy = to.y() - from.y();
    return std::hypot(dx, dy) / speed;
}

void VdaRobotCommandHandle::follow_new_path(
    const std::vector<rmf_traffic::agv::Plan::Waypoint> &waypoints,
    ArrivalEstimator next_arrival_estimator,
    RequestCompleted path_finished_callback)
{
    if (waypoints.empty())
    {
        RCLCPP_INFO(_logger, "[%s] follow_new_path with no waypoints -- nothing to do",
                    _name.c_str());
        if (path_finished_callback)
        {
            path_finished_callback();
        }
        return;
    }

    std::vector<Connector::RoutePoint> route;
    ActivePath active;
    route.reserve(waypoints.size());
    active.node_ids.reserve(waypoints.size());
    active.positions.reserve(waypoints.size());
    active.times.reserve(waypoints.size());

    std::string map_name;
    for (const auto &wp : waypoints)
    {
        const Eigen::Vector3d p = wp.position();
        const std::string node_id = node_id_for(wp);

        if (_graph && wp.graph_index().has_value())
        {
            const auto &wp_map = _graph->get_waypoint(*wp.graph_index()).get_map_name();
            if (!map_name.empty() && map_name != wp_map)
            {
                // The current order builder supports one map identifier per route.
                RCLCPP_WARN(_logger,"[%s] path spans maps '%s' and '%s'; sending it as one "
                            "order on '%s', which the AGV may reject", _name.c_str(), map_name.c_str(), wp_map.c_str(), wp_map.c_str());
            }
            map_name = wp_map;
        }

        route.push_back(Connector::RoutePoint{node_id, p.x(), p.y(), p.z(), lane_speed_limit(wp)});
        active.node_ids.push_back(node_id);
        active.positions.push_back(p);
        active.times.push_back(wp.time());
    }

    if (map_name.empty())
    {
        // Fall back to the map in the latest AGV state.
        const auto data = _connector.get_data(_name);
        if (data.has_value())
        {
            map_name = data->map_name;
        }
    }

    active.next_index = 0;
    active.arrival_estimator = std::move(next_arrival_estimator);
    active.finished = std::move(path_finished_callback);

    RCLCPP_INFO(_logger, "[%s] follow_new_path: %zu waypoint(s) on '%s', ending at '%s'",
                _name.c_str(), route.size(), map_name.c_str(), route.back().node_id.c_str());

    // Remove callbacks associated with the superseded path.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _path.reset();
    }

    // Store the path together with the orderId returned by the connector.
    const auto result = _connector.navigate_route(_name, route, map_name);
    if (result.status == CommandStatus::transport_failed)
    {
        // Do not report completion for a command that was not dispatched.
        RCLCPP_ERROR(_logger,"[%s] follow_new_path: order was not published (transport failure) -- "
                     "RMF will see no progress on this command",_name.c_str());
        return;
    }
    active.order_id = result.order_id;

    std::lock_guard<std::mutex> lock(_mutex);
    _path = std::move(active);
}

void VdaRobotCommandHandle::stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // A stopped path must not invoke its completion callback.
        _path.reset();
    }
    const auto status = _connector.stop(_name);
    if (status == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger,"[%s] stop: cancelOrder was not published (transport failure) -- the "
                    "AGV was not told to stop and may still be executing its last order", _name.c_str());
        return;
    }
    RCLCPP_INFO(_logger, "[%s] stop", _name.c_str());
}

void VdaRobotCommandHandle::dock(const std::string &dock_name,
                                 RequestCompleted docking_finished_callback)
{
    const std::string action_id = _connector.execute_instant_action(_name, dock_name);

    if (action_id.empty())
    {
        // Do not report docking completion when dispatch fails.
        RCLCPP_ERROR(_logger, "[%s] dock '%s' was not published (unregistered robot or transport " "failure) -- RMF will see no progress on this dock",
                    _name.c_str(), dock_name.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    RCLCPP_INFO(_logger, "[%s] dock '%s' (action %s)", _name.c_str(), dock_name.c_str(), action_id.c_str());
    _dock_action_id = action_id;
    _dock_finished = std::move(docking_finished_callback);
}

void VdaRobotCommandHandle::on_perform_action(const std::string &category,const nlohmann::json &description,
                                              RobotUpdateHandle::ActionExecution execution)
{
    RCLCPP_INFO(_logger, "[%s] action '%s'", _name.c_str(), category.c_str());

    // Preserve the JSON types of action parameter values.
    const nlohmann::json params = description.is_object() ? description : nlohmann::json::object();
    const std::string action_id = _connector.execute_instant_action(_name, category, params);
    if (action_id.empty())
    {
        RCLCPP_ERROR(_logger,"[%s] action '%s' was not published - robot is not registered "
                     "with the VDA5050 connector", _name.c_str(), category.c_str());
        execution.error("Unable to dispatch VDA5050 action");
        return;
    }

    std::optional<RobotUpdateHandle::ActionExecution> superseded;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_action_exec.has_value())
        {
            // Explicitly terminate tracking for a superseded action.
            RCLCPP_WARN(_logger,"[%s] action '%s' dispatched while action %s was still tracked -- " "abandoning the earlier one",
                        _name.c_str(), action_id.c_str(), _action_id.c_str());
            superseded = std::move(_action_exec);
        }
        _action_id = action_id;
        _action_exec = std::move(execution);
    }
    // RMF callbacks may re-enter this object and must run outside the lock.
    if (superseded.has_value())
    {
        superseded->error("Superseded by another action before finishing");
    }
}

void VdaRobotCommandHandle::update(const RobotData &data)
{
    // Snapshot the update handle under the command-state lock.
    std::shared_ptr<RobotUpdateHandle> handle;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        handle = _update_handle;
    }
    if (!handle)
    {
        return;
    }

    // Publish pose and battery telemetry before command progress.
    handle->update_position(data.map_name,Eigen::Vector3d(data.position[0], data.position[1], data.position[2]));
    handle->update_battery_soc(data.battery_soc);

    // Update commission readiness from the latest AGV state.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _ready_for_orders = data.ready_for_orders();
        if (!data.operable)
        {
            _not_ready_reason = "operating mode " + data.operating_mode;
        }
        else if (data.safety_state.field_violation)
        {
            _not_ready_reason = "protective field violated";
        }
        else if (data.safety_state.triggered())
        {
            _not_ready_reason = "eStop " + data.safety_state.e_stop;
        }
        else if (!data.fatal_error.empty())
        {
            _not_ready_reason = "FATAL error '" + data.fatal_error + "'";
        }
        else
        {
            _not_ready_reason.clear();
        }
    }
    apply_commission();

    RequestCompleted path_done;
    RequestCompleted dock_done;
    bool dock_failed = false;
    std::string dock_action_id_done;
    ArrivalEstimator estimator;
    std::size_t estimate_index = 0;
    double estimate_seconds_left = 0.0;
    bool have_estimate = false;
    std::optional<RobotUpdateHandle::ActionExecution> action_exec;
    std::string action_id_done;
    bool action_failed = false;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (!_dock_action_id.empty())
        {
            const auto status = _connector.get_action_state(_name, _dock_action_id);
            if (status.has_value() && vda5050::is_terminal_action_status(*status))
            {
                dock_failed = (status == "FAILED");
                dock_action_id_done = _dock_action_id;
                _dock_action_id.clear();
                if (dock_failed)
                {
                    // RequestCompleted cannot represent docking failure.
                    _dock_finished = nullptr;
                }
                else
                {
                    dock_done = std::move(_dock_finished);
                    _dock_finished = nullptr;
                }
            }
        }

        if (_action_exec.has_value())
        {
            // Stop tracking an action withdrawn by RMF. VDA5050 does not
            // define cancellation for an instant action already dispatched.
            if (!_action_exec->okay())
            {
                RCLCPP_WARN(_logger,
                            "[%s] action %s was stopped by RMF -- the AGV may still be "
                            "executing it, VDA5050 offers no way to cancel it",
                            _name.c_str(), _action_id.c_str());
                _action_exec.reset();
                _action_id.clear();
            }
            else
            {
                const auto status = _connector.get_action_state(_name, _action_id);
                if (status.has_value() && vda5050::is_terminal_action_status(*status))
                {
                    action_failed = (status == "FAILED");
                    action_id_done = _action_id;
                    action_exec = std::move(_action_exec);
                    _action_exec.reset();
                    _action_id.clear();
                }
            }
        }

        if (_path.has_value())
        {
            auto &path = *_path;

            const Eigen::Vector3d here(data.position[0], data.position[1],
                                       data.position[2]);

            // Route nodes use sequenceId 2*(i+1); accept sequence progress only while the AGV reports the matching orderId.
            if (data.last_node_sequence_id.has_value() && !path.order_id.empty() &&
                data.order_id == path.order_id)
            {
                const std::size_t passed = static_cast<std::size_t>(*data.last_node_sequence_id) / 2;
                // Keep progress monotonic.
                path.next_index = std::max(path.next_index, std::min(passed, path.node_ids.size()));
            }

            // Advance through all waypoints confirmed by nodeId or proximity.
            while (path.next_index < path.node_ids.size())
            {
                const bool reported = !data.last_node_id.empty() &&
                    path.node_ids[path.next_index] == data.last_node_id;

                const auto &target = path.positions[path.next_index];
                const bool standing_on_it = std::hypot(target.x() - here.x(), target.y() - here.y()) <= kWaypointReachedMetres;

                if (!reported && !standing_on_it)
                {
                    break;
                }

                // Compare planned and actual arrival using the RMF clock.
                if (path.next_index < path.times.size() && _clock)
                {
                    const double early = std::chrono::duration<double>(
                        path.times[path.next_index] - rmf_traffic_ros2::convert(_clock->now())).count();

                    if (early > kEarlyArrivalWarnSeconds)
                    {
                        RCLCPP_WARN(_logger,"[%s] reached waypoint %zu ~%.1fs ahead of the time RMF "
                                    "planned around -- other itineraries assumed this robot "
                                    "would not be here yet",_name.c_str(), path.next_index, early);
                    }
                }
                ++path.next_index;
            }

            if (_connector.is_command_completed(_name))
            {
                RCLCPP_INFO(_logger, "[%s] path completed", _name.c_str());
                path_done = std::move(path.finished);
                _path.reset();
            }
            else if (path.next_index < path.positions.size() && path.arrival_estimator)
            {
                estimator = path.arrival_estimator;
                estimate_index = path.next_index;
                estimate_seconds_left = estimate_seconds(here, path.positions[path.next_index], data.velocity);
                have_estimate = true;
            }
        }
    }

    // RMF callbacks may re-enter this object and must run outside the lock.
    if (have_estimate && estimator)
    {
        estimator(estimate_index,std::chrono::duration_cast<rmf_traffic::Duration>(std::chrono::duration<double>(estimate_seconds_left)));
    }
    if (dock_failed)
    {
        RCLCPP_ERROR(_logger,"[%s] dock action %s FAILED -- RMF will see no progress on this dock",
                     _name.c_str(), dock_action_id_done.c_str());
    }
    if (dock_done)
    {
        dock_done();
    }
    if (action_exec.has_value())
    {
        if (action_failed)
        {
            RCLCPP_ERROR(_logger, "[%s] action %s FAILED", _name.c_str(), action_id_done.c_str());
            action_exec->error("VDA5050 action reported FAILED");
        }
        else
        {
            RCLCPP_INFO(_logger, "[%s] action %s finished", _name.c_str(), action_id_done.c_str());
            action_exec->finished();
        }
    }
    if (path_done)
    {
        path_done();
    }
}

void VdaRobotCommandHandle::set_update_handle(std::shared_ptr<RobotUpdateHandle> handle)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _update_handle = handle;
    }
    if (!handle)
    {
        return;
    }

    // Avoid extending this command handle's lifetime through RMF's executor.
    std::weak_ptr<VdaRobotCommandHandle> weak = weak_from_this();
    handle->set_action_executor([weak](const std::string &category, const nlohmann::json &description, RobotUpdateHandle::ActionExecution execution)
        {
            if (const auto self = weak.lock())
            {
                self->on_perform_action(category, description, std::move(execution));
            }
        });
}

bool VdaRobotCommandHandle::added() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return static_cast<bool>(_update_handle);
}

std::string VdaRobotCommandHandle::pause()
{
    std::shared_ptr<RobotUpdateHandle> handle;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_update_handle)
        {
            return "robot is not in the RMF fleet yet";
        }
        if (_paused)
        {
            return "already paused";
        }
        handle = _update_handle;
    }

    // Preserve the active order by avoiding RMF interruption, which invokes stop(). The delay ceiling is restored by resume().
    const auto saved_delay = handle->maximum_delay();
    handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());

    const auto status = _connector.pause(_name);
    if (status == CommandStatus::transport_failed)
    {
        // Restore RMF state when startPause is not dispatched.
        handle->maximum_delay(saved_delay);
        RCLCPP_ERROR(_logger, "[%s] pause: startPause did not reach the AGV (transport " "failure)", _name.c_str());
        return "startPause failed to reach the AGV (MQTT publish failed)";
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _saved_maximum_delay = saved_delay;
        _paused = true;
    }
    RCLCPP_INFO(_logger, "[%s] paused by operator", _name.c_str());
    return {};
}

std::string VdaRobotCommandHandle::resume()
{
    std::shared_ptr<RobotUpdateHandle> handle;
    rmf_utils::optional<rmf_traffic::Duration> saved_delay;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_paused)
        {
            return "not paused";
        }
        handle = _update_handle;
        saved_delay = _saved_maximum_delay;
    }

    // Resume the AGV before restoring RMF's delay ceiling.
    const auto status = _connector.resume(_name);
    if (status == CommandStatus::transport_failed)
    {
        // Preserve pause state when stopPause is not dispatched.
        RCLCPP_ERROR(_logger, "[%s] resume: stopPause did not reach the AGV (transport " "failure) -- still paused", _name.c_str());

        return "stopPause failed to reach the AGV (MQTT publish failed) -- still paused";
    }

    if (handle)
    {
        handle->maximum_delay(saved_delay);
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _paused = false;
    }
    RCLCPP_INFO(_logger, "[%s] resumed by operator", _name.c_str());
    return {};
}

void VdaRobotCommandHandle::set_online(bool online)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_online == online)
        {
            return;
        }
        _online = online;
    }
    apply_commission();
}

void VdaRobotCommandHandle::apply_commission()
{
    // Apply RMF commission updates outside the command-state lock.
    std::shared_ptr<RobotUpdateHandle> handle;
    bool commission = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_update_handle)
        {
            return;
        }
        // Commission only robots that are online and ready for orders.
        commission = _online.value_or(false) && _ready_for_orders;
        if (_commissioned == commission)
        {
            return;
        }
        _commissioned = commission;
        handle = _update_handle;
        reason = !_online.value_or(false) ? "no recent VDA5050 state" : _not_ready_reason;
    }

    if (commission)
    {
        handle->set_commission(RobotUpdateHandle::Commission());
        RCLCPP_INFO(_logger, "[%s] available again -- recommissioned with RMF", _name.c_str());
        return;
    }

    handle->set_commission(RobotUpdateHandle::Commission::decommission());
    RCLCPP_WARN(_logger,"[%s] %s -- decommissioned, RMF will not dispatch new tasks to it", _name.c_str(), reason.c_str());
}

}  // namespace vda5050_fleet_adapter_full_control::rmf
