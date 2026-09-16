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

// Treat near-identical poses as the same waypoint.
constexpr double kSamePoseMetres = 0.05;
constexpr double kSamePoseRadians = 0.05;

// Allow this long for a new path before a traffic hold becomes a cancellation.
constexpr double kTrafficPauseTimeoutSeconds = 10.0;

// Count route points whose release time has passed; the first point is always released.
std::size_t releasable_count(const std::vector<rmf_traffic::Time> &times, rmf_traffic::Time now)
{
    if (times.empty())
    {
        return 0;
    }
    std::size_t released = 1;
    while (released < times.size() && now >= times[released - 1])
    {
        ++released;
    }
    return released;
}

// Whether the graph has a lane directly from waypoint `from` to `to`.
bool has_lane(const rmf_traffic::agv::Graph &graph, std::size_t from, std::size_t to)
{
    for (std::size_t i = 0; i < graph.num_lanes(); ++i)
    {
        const auto &lane = graph.get_lane(i);
        if (lane.entry().waypoint_index() == from && lane.exit().waypoint_index() == to)
        {
            return true;
        }
    }
    return false;
}
}  // namespace

VdaRobotCommandHandle::VdaRobotCommandHandle(
    rclcpp::Logger logger, std::string name, Connector &connector,
    std::shared_ptr<const rmf_traffic::agv::Graph> graph, double nominal_speed,
    rclcpp::Clock::SharedPtr clock, bool honor_waypoint_timing)
  : _logger(std::move(logger)), _name(std::move(name)), _connector(connector),
    _graph(std::move(graph)),
    _nominal_speed(nominal_speed > 0.0 ? nominal_speed : 0.5),
    _clock(std::move(clock)),
    _honor_waypoint_timing(honor_waypoint_timing)
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
        // Handle unnamed graph waypoints.
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
    // Use measured speed while moving, otherwise the fleet's nominal speed.
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

    // Skip the first RMF waypoint only when its pose matches the AGV's current pose.
    std::size_t start_index = 0;
    const auto current = _connector.get_data(_name);
    if (current.has_value())
    {
        const Eigen::Vector3d first = waypoints.front().position();
        double dtheta = first.z() - current->position[2];
        dtheta = std::atan2(std::sin(dtheta), std::cos(dtheta));
        const double dxy = std::hypot(first.x() - current->position[0], first.y() - current->position[1]);
        if (dxy < kSamePoseMetres && std::fabs(dtheta) < kSamePoseRadians)
        {
            start_index = 1;
        }
    }

    if (start_index >= waypoints.size())
    {
        RCLCPP_INFO(_logger, "[%s] follow_new_path: already at the only waypoint given, nothing to " "travel to", _name.c_str());
        if (path_finished_callback)
        {
            path_finished_callback();
        }
        return;
    }

    if (_honor_waypoint_timing)
    {
        // Restore the configured delay limit for each new RMF command.
        std::shared_ptr<RobotUpdateHandle> handle;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            handle = _update_handle;
        }
        if (handle)
        {
            handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());
        }
    }

    std::vector<Connector::RoutePoint> route;
    ActivePath active;
    active.waypoint_offset = start_index;
    const std::size_t route_size = waypoints.size() - start_index;
    route.reserve(route_size);
    active.node_ids.reserve(route_size);
    active.positions.reserve(route_size);
    active.times.reserve(route_size);

    std::string map_name;
    for (std::size_t i = start_index; i < waypoints.size(); ++i)
    {
        const auto &wp = waypoints[i];
        const Eigen::Vector3d p = wp.position();
        const std::string node_id = node_id_for(wp);

        if (_graph && wp.graph_index().has_value())
        {
            const auto &wp_map = _graph->get_waypoint(*wp.graph_index()).get_map_name();
            if (!map_name.empty() && map_name != wp_map)
            {
                // Use one map ID for the entire VDA5050 order.
                RCLCPP_WARN(_logger,
                            "[%s] path spans maps '%s' and '%s'; sending it as one "
                            "order on '%s', which the AGV may reject",
                            _name.c_str(), map_name.c_str(), wp_map.c_str(),
                            wp_map.c_str());
            }
            map_name = wp_map;

            if (i > start_index && waypoints[i - 1].graph_index().has_value() &&
                !has_lane(*_graph, *waypoints[i - 1].graph_index(), *wp.graph_index()))
            {
                RCLCPP_WARN(_logger, "[%s] no graph lane from '%s' to '%s' in this order",
                            _name.c_str(), active.node_ids.back().c_str(), node_id.c_str());
            }
        }

        route.push_back(
            Connector::RoutePoint{node_id, p.x(), p.y(), p.z(), lane_speed_limit(wp)});
        active.node_ids.push_back(node_id);
        active.positions.push_back(p);
        active.times.push_back(wp.time());
    }

    if (map_name.empty())
    {
        // Use the AGV's latest map when the route has no map ID.
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

    // Clear the old path before publishing a new order.
    bool had_active_path = false;
    bool resuming_from_pause = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        had_active_path = _path.has_value();
        _path.reset();
        resuming_from_pause = _traffic_pause_deadline.has_value();
        _traffic_pause_deadline.reset();
    }
    if (resuming_from_pause)
    {
        RCLCPP_INFO(_logger,
                    "[%s] follow_new_path: resuming after a traffic hold -- updating the "
                    "order and sending stopPause instead of cancelling it",
                    _name.c_str());
    }
    else if (had_active_path)
    {
        RCLCPP_INFO(_logger,
                    "[%s] follow_new_path: superseding an order still in progress -- "
                    "cancelling it before publishing the replacement",
                    _name.c_str());
        if (_connector.stop(_name) == CommandStatus::transport_failed)
        {
            RCLCPP_WARN(_logger,
                        "[%s] follow_new_path: cancelOrder was not published -- the AGV "
                        "may still be executing the order this one is replacing",
                        _name.c_str());
        }
    }

    std::optional<std::size_t> initial_release;
    if (_honor_waypoint_timing && _clock)
    {
        active.released_count =
            releasable_count(active.times, rmf_traffic_ros2::convert(_clock->now()));
        initial_release = active.released_count;
    }
    else
    {
        active.released_count = route.size();
    }

    const auto result = _connector.navigate_route(_name, route, map_name, initial_release);
    if (result.status == CommandStatus::transport_failed)
    {
        // Keep the path inactive when its order was never published.
        RCLCPP_ERROR(_logger,
                     "[%s] follow_new_path: order was not published (transport failure) -- "
                     "RMF will see no progress on this command",
                     _name.c_str());
        return;
    }
    active.order_id = result.order_id;

    if (resuming_from_pause && _connector.resume(_name) == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger,
                     "[%s] follow_new_path: stopPause did not reach the AGV -- it may "
                     "still be paused",
                     _name.c_str());
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _path = std::move(active);
}

void VdaRobotCommandHandle::stop()
{
    // Pause on stop and cancel only if no replacement path arrives.
    if (_connector.pause(_name) == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger,
                     "[%s] stop: startPause did not reach the AGV (transport failure) -- "
                     "the AGV may still be moving",
                     _name.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _traffic_pause_deadline = std::chrono::steady_clock::now() +  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                                     std::chrono::duration<double>(kTrafficPauseTimeoutSeconds));
    RCLCPP_INFO(_logger, "[%s] stop (startPause, awaiting resume or cancel)", _name.c_str());
}

void VdaRobotCommandHandle::dock(const std::string &dock_name, RequestCompleted docking_finished_callback)
{
    const std::string action_id = _connector.execute_instant_action(_name, dock_name);

    if (action_id.empty())
    {
        // Do not complete an action that was never dispatched.
        RCLCPP_ERROR(_logger,
                     "[%s] dock '%s' was not published (unregistered robot or transport "
                     "failure) -- RMF will see no progress on this dock",
                     _name.c_str(), dock_name.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    RCLCPP_INFO(_logger, "[%s] dock '%s' (action %s)", _name.c_str(), dock_name.c_str(), action_id.c_str());
    _dock_action_id = action_id;
    _dock_finished = std::move(docking_finished_callback);
}

void VdaRobotCommandHandle::on_perform_action(const std::string &category, const nlohmann::json &description, RobotUpdateHandle::ActionExecution execution)
{
    RCLCPP_INFO(_logger, "[%s] action '%s'", _name.c_str(), category.c_str());

    // Preserve each action parameter's JSON type.
    const nlohmann::json params = description.is_object() ? description : nlohmann::json::object();
    const std::string action_id = _connector.execute_instant_action(_name, category, params);
    if (action_id.empty())
    {
        RCLCPP_ERROR(_logger, "[%s] action '%s' was not published - robot is not registered "  "with the VDA5050 connector", _name.c_str(), category.c_str());
        execution.error("Unable to dispatch VDA5050 action");
        return;
    }

    std::optional<RobotUpdateHandle::ActionExecution> superseded;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_action_exec.has_value())
        {
            // Complete the previous action before tracking a new one.
            RCLCPP_WARN(_logger, "[%s] action '%s' dispatched while action %s was still tracked -- " "abandoning the earlier one",  _name.c_str(), action_id.c_str(), _action_id.c_str());
            superseded = std::move(_action_exec);
        }
        _action_id = action_id;
        _action_exec = std::move(execution);
    }
    // Call RMF outside the command lock to allow callbacks.
    if (superseded.has_value())
    {
        superseded->error("Superseded by another action before finishing");
    }
}

void VdaRobotCommandHandle::update(const RobotData &data)
{
    // Read the update handle under the command lock.
    std::shared_ptr<RobotUpdateHandle> handle;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        handle = _update_handle;
    }
    if (!handle)
    {
        return;
    }

    handle->update_position(
        data.map_name, Eigen::Vector3d(data.position[0], data.position[1], data.position[2]));
    handle->update_battery_soc(data.battery_soc);

    // Sync the pause flag with the AGV's reported state.
    bool was_paused;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        was_paused = _paused;
    }
    if (data.paused != was_paused)
    {
        if (data.paused)
        {
            const auto saved_delay = handle->maximum_delay();
            handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());
            std::lock_guard<std::mutex> lock(_mutex);
            _saved_maximum_delay = saved_delay;
            _paused = true;
        }
        else
        {
            rmf_utils::optional<rmf_traffic::Duration> saved_delay;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                saved_delay = _saved_maximum_delay;
            }
            handle->maximum_delay(saved_delay);
            std::lock_guard<std::mutex> lock(_mutex);
            _paused = false;
        }
        RCLCPP_INFO(_logger, "[%s] AGV pause state changed to %s outside pause()/resume() -- syncing RMF",  _name.c_str(), data.paused ? "paused" : "not paused");
    }

    // Cancel a traffic hold that received no replacement path.
    bool escalate_to_cancel = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_traffic_pause_deadline && std::chrono::steady_clock::now() >= *_traffic_pause_deadline)
        {
            _traffic_pause_deadline.reset();
            _path.reset();
            escalate_to_cancel = true;
        }
    }
    if (escalate_to_cancel)
    {
        RCLCPP_WARN(_logger, "[%s] stop: no resume arrived -- cancelling for real", _name.c_str());
        _connector.stop(_name);
    }

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
        else if (data.paused)
        {
            _not_ready_reason = "AGV reports paused";
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
    bool trigger_replan = false;
    std::size_t grow_release_to = 0;
    std::string grow_order_id;

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
            if (!_action_exec->okay())
            {
                // A dispatched instant action may continue after RMF withdraws it.
                RCLCPP_WARN(_logger, "[%s] action %s was stopped by RMF -- the AGV may still be " "executing it, VDA5050 offers no way to cancel it", _name.c_str(), _action_id.c_str());
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

            // Release route points as their scheduled times pass.
            if (_honor_waypoint_timing && _clock && path.released_count < path.times.size())
            {
                const auto now = rmf_traffic_ros2::convert(_clock->now());
                const std::size_t releasable = releasable_count(path.times, now);
                if (releasable > path.released_count)
                {
                    grow_release_to = releasable;
                    grow_order_id = path.order_id;
                }
            }

            const Eigen::Vector3d here(data.position[0], data.position[1], data.position[2]);

            // Match sequence progress only to the active order ID.
            if (data.last_node_sequence_id.has_value() && !path.order_id.empty() &&
                data.order_id == path.order_id)
            {
                const std::size_t passed =
                    static_cast<std::size_t>(*data.last_node_sequence_id) / 2;
                path.next_index =
                    std::max(path.next_index, std::min(passed, path.node_ids.size()));
            }

            // Advance progress when the AGV reports or reaches a waypoint.
            while (path.next_index < path.node_ids.size())
            {
                const auto &target = path.positions[path.next_index];

                const bool reported =  !data.last_node_id.empty() && path.node_ids[path.next_index] == data.last_node_id;
                const bool standing_on_it = std::hypot(target.x() - here.x(), target.y() - here.y()) <= kWaypointReachedMetres;
                bool reached = reported || standing_on_it;

                // Keep repeated node IDs for in-place turns.
                if (reached && path.next_index > 0 &&
                    path.node_ids[path.next_index] == path.node_ids[path.next_index - 1])
                {
                    double dtheta = target.z() - here.z();
                    dtheta = std::atan2(std::sin(dtheta), std::cos(dtheta));
                    reached = std::fabs(dtheta) < kSamePoseRadians;
                }

                if (!reached)
                {
                    break;
                }

                // Report early arrival against RMF's schedule.
                if (path.next_index < path.times.size() && _clock)
                {
                    const double early = std::chrono::duration<double>( path.times[path.next_index] - rmf_traffic_ros2::convert(_clock->now())).count();

                    if (early > kEarlyArrivalWarnSeconds)
                    {
                        RCLCPP_WARN(_logger, "[%s] reached waypoint %zu ~%.1fs ahead of the time RMF " "planned around -- other itineraries assumed this robot "
                                    "would not be here yet",  _name.c_str(), path.next_index, early);
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
            else if (!path.replan_requested && _connector.is_order_stuck(_name))
            {
                // Request a replan when the AGV does not accept the order in time.
                path.replan_requested = true;
                trigger_replan = true;
            }
            else if (path.next_index < path.positions.size() && path.arrival_estimator)
            {
                estimator = path.arrival_estimator;
                // Restore the RMF path index after dropping a redundant first waypoint.
                estimate_index = path.next_index + path.waypoint_offset;

                // Use the next release time for points still in the horizon.
                const bool gated_by_horizon = _honor_waypoint_timing && _clock &&  path.next_index >= path.released_count && path.next_index < path.times.size();
                if (gated_by_horizon)
                {
                    estimate_seconds_left = std::max(0.0, std::chrono::duration<double>( path.times[path.next_index] - rmf_traffic_ros2::convert(_clock->now())).count());
                }
                else
                {
                    estimate_seconds_left = estimate_seconds(here, path.positions[path.next_index], data.velocity);
                }
                have_estimate = true;
            }
        }
    }

    // Invoke RMF callbacks outside the command lock.
    if (trigger_replan)
    {
        RCLCPP_WARN(_logger, "[%s] order unacknowledged for too long -- asking RMF to replan",_name.c_str());
        handle->replan();
    }
    if (grow_release_to > 0 &&
        _connector.release_more(_name, grow_release_to) != CommandStatus::transport_failed)
    {
        // Apply horizon growth only while this order remains active.
        std::lock_guard<std::mutex> lock(_mutex);
        if (_path.has_value() && _path->order_id == grow_order_id)
        {
            _path->released_count = grow_release_to;
        }
    }
    if (have_estimate && estimator)
    {
        estimator(estimate_index, std::chrono::duration_cast<rmf_traffic::Duration>(std::chrono::duration<double>(estimate_seconds_left)));
    }
    if (dock_failed)
    {
        // Request a replan when the active path cannot complete.
        RCLCPP_ERROR(_logger, "[%s] dock action %s FAILED -- asking RMF to replan",  _name.c_str(), dock_action_id_done.c_str());
        handle->replan();
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

    if (_honor_waypoint_timing)
    {
        // Treat a stop at the release boundary as a scheduled hold.
        handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());
    }

    // Keep the executor tied to the robot's RMF registration.
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

    // Pause without cancelling the order and lift the delay ceiling.
    const auto saved_delay = handle->maximum_delay();
    handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());

    const auto status = _connector.pause(_name);
    if (status == CommandStatus::transport_failed)
    {
        // Restore the delay ceiling if the pause action fails.
        handle->maximum_delay(saved_delay);
        RCLCPP_ERROR(_logger, "[%s] pause: startPause did not reach the AGV (transport failure)", _name.c_str());
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

    // Resume the AGV before restoring the delay ceiling.
    const auto status = _connector.resume(_name);
    if (status == CommandStatus::transport_failed)
    {
        // Keep the pause state if stopPause fails.
        RCLCPP_ERROR(_logger,
                     "[%s] resume: stopPause did not reach the AGV (transport failure) -- "
                     "still paused",
                     _name.c_str());
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

void VdaRobotCommandHandle::set_ready_for_orders(bool ready, const std::string &reason)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ready_for_orders == ready)
        {
            return;
        }
        _ready_for_orders = ready;
        if (!ready)
        {
            _not_ready_reason = reason;
        }
    }
    apply_commission();
}

void VdaRobotCommandHandle::apply_commission()
{
    // Apply commission changes outside the command lock.
    std::shared_ptr<RobotUpdateHandle> handle;
    bool commission = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_update_handle)
        {
            return;
        }
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
    RCLCPP_WARN(_logger, "[%s] %s -- decommissioned, RMF will not dispatch new tasks to it", _name.c_str(), reason.c_str());
}

}  // namespace vda5050_fleet_adapter_full_control::rmf
