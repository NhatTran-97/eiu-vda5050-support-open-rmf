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
}  // namespace

VdaRobotCommandHandle::VdaRobotCommandHandle( const rclcpp::Logger &logger, std::string name, Connector &connector,
    std::shared_ptr<const rmf_traffic::agv::Graph> graph, double nominal_speed, rclcpp::Clock::SharedPtr clock, bool honor_waypoint_timing, bool stitch_on_replan, const RoutePolicy &route_policy)
  : _logger(logger), _name(std::move(name)), _connector(connector),
    _graph(std::move(graph)),
    _nominal_speed(nominal_speed > 0.0 ? nominal_speed : 0.5),
    _clock(std::move(clock)),
    _honor_waypoint_timing(honor_waypoint_timing),
    _stitch_on_replan(stitch_on_replan),
    _route_policy(route_policy)
{
}

std::string VdaRobotCommandHandle::derive_node_id(const std::string &name, std::optional<std::size_t> graph_index, double x, double y)
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
    const Eigen::Vector3d &p = wp.position();
    std::string name;
    if (_graph && wp.graph_index().has_value())
    {
        // Take the waypoint's name from the nav graph; an unnamed waypoint keeps an empty name.
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
    if (velocity.has_value() && velocity->speed() >= _route_policy.usable_speed_mps)
    {
        speed = velocity->speed();
    }

    const double dx = to.x() - from.x();
    const double dy = to.y() - from.y();
    return std::hypot(dx, dy) / speed;
}

rmf_utils::optional<rmf_traffic::Duration> VdaRobotCommandHandle::timed_release_delay_limit() const
{
    if (_route_policy.timed_release_max_delay_s <= 0.0)
    {
        return rmf_utils::optional<rmf_traffic::Duration>();
    }
    return std::chrono::duration_cast<rmf_traffic::Duration>(
        std::chrono::duration<double>(_route_policy.timed_release_max_delay_s));
}

void VdaRobotCommandHandle::schedule_replan()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_replan_at.has_value())
    {
        _replan_at = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(_route_policy.replan_after_s));
    }
}

void VdaRobotCommandHandle::follow_new_path(const std::vector<rmf_traffic::agv::Plan::Waypoint> &waypoints, ArrivalEstimator next_arrival_estimator, RequestCompleted path_finished_callback)
{
    std::unique_lock<std::mutex> command(_command_mutex);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_retired)
        {
            RCLCPP_WARN(_logger, "[%s] follow_new_path: the robot was removed from the fleet -- ignoring the path", _name.c_str());
            return;
        }
    }

    if (waypoints.empty())
    {
        RCLCPP_INFO(_logger, "[%s] follow_new_path with no waypoints -- nothing to do",_name.c_str());
        command.unlock();
        if (path_finished_callback)
        {
            path_finished_callback();
        }
        return;
    }

    // An AGV without a valid pose cannot start from anywhere, so no order goes out until it has one.
    const auto current = _connector.get_data(_name);
    if (!current.has_value())
    {
        RCLCPP_WARN(_logger, "[%s] follow_new_path: the AGV has no valid pose -- not sending the order, replanning in %.0f s", _name.c_str(), _route_policy.replan_after_s);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _path.reset();
            _pending_order.reset();
        }
        if (_connector.order_in_progress(_name) && _connector.stop(_name) == CommandStatus::transport_failed)
        {
            RCLCPP_WARN(_logger, "[%s] follow_new_path: cancelOrder for the replaced order was not published", _name.c_str());
        }
        schedule_replan();
        return;
    }

    // Skip the first RMF waypoint only when its pose matches the AGV's current pose.
    std::size_t start_index = 0;
    {
        const Eigen::Vector3d first = waypoints.front().position();
        double dtheta = first.z() - current->position[2];
        dtheta = std::atan2(std::sin(dtheta), std::cos(dtheta));
        const double dxy = std::hypot(first.x() - current->position[0], first.y() - current->position[1]);
        if (dxy < _route_policy.same_pose_m && std::fabs(dtheta) < _route_policy.same_pose_rad)
        {
            start_index = 1;
        }
    }

    if (start_index >= waypoints.size())
    {
        RCLCPP_INFO(_logger, "[%s] follow_new_path: already at the only waypoint given, nothing to " "travel to", _name.c_str());
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _path.reset();
            _pending_order.reset();
            _replan_at.reset();
        }
        if (_connector.order_in_progress(_name) && _connector.stop(_name) == CommandStatus::transport_failed)
        {
            RCLCPP_WARN(_logger, "[%s] follow_new_path: cancelOrder for the replaced order was not published", _name.c_str());
        }
        end_traffic_hold();
        command.unlock();
        if (path_finished_callback)
        {
            path_finished_callback();
        }
        return;
    }

    if (_honor_waypoint_timing)
    {
        // A paused robot keeps the delay ceiling its pause lifted.
        std::shared_ptr<RobotUpdateHandle> handle;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            handle = _paused ? nullptr : _update_handle;
        }
        if (handle)
        {
            handle->maximum_delay(timed_release_delay_limit());
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
    active.targets.reserve(route_size);

    std::string map_name;
    std::string level;
    for (std::size_t i = start_index; i < waypoints.size(); ++i)
    {
        const auto &wp = waypoints[i];
        const Eigen::Vector3d &p = wp.position();
        const std::string node_id = node_id_for(wp);

        if (_graph && wp.graph_index().has_value())
        {
            const auto &wp_map = _graph->get_waypoint(*wp.graph_index()).get_map_name();
            if (!level.empty() && level != wp_map)
            {
                RCLCPP_WARN(_logger, "[%s] path changes level from '%s' to '%s' at '%s'", _name.c_str(), level.c_str(), wp_map.c_str(), node_id.c_str());
            }
            level = wp_map;
            if (map_name.empty())
            {
                map_name = wp_map;
            }

            // Warn when RMF gives no approach lane and no lane links the two waypoints directly.
            if (i > start_index && waypoints[i - 1].graph_index().has_value() && waypoints[i - 1].graph_index() != wp.graph_index() &&
                wp.approach_lanes().empty() && !_graph->lane_from(*waypoints[i - 1].graph_index(), *wp.graph_index()))
            {
                RCLCPP_WARN(_logger, "[%s] no graph lane from '%s' to '%s' in this order", _name.c_str(), active.node_ids.back().c_str(), node_id.c_str());
            }
        }

        route.push_back(Connector::RoutePoint{node_id, p.x(), p.y(), p.z(), edge_speed_limit(wp), level});
        active.node_ids.push_back(node_id);
        active.positions.push_back(p);
        active.times.push_back(wp.time());
        active.targets.push_back(RouteTarget{wp.graph_index(), wp.approach_lanes()});
    }

    if (map_name.empty())
    {
        // Use the AGV's latest map when the route has no map ID.
        map_name = current->map_name;
    }

    active.next_index = 0;
    active.arrival_estimator = std::move(next_arrival_estimator);
    active.finished = std::move(path_finished_callback);

    RCLCPP_INFO(_logger, "[%s] follow_new_path: %zu waypoint(s) on '%s', ending at '%s'", _name.c_str(), route.size(), map_name.c_str(), route.back().node_id.c_str());

    // The old path ends here; a held order stays held until the new one is out.
    bool had_active_path = false;
    bool holding = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        had_active_path = _path.has_value();
        _path.reset();
        _pending_order.reset();
        holding = _hold == Hold::held;
    }
    std::optional<std::size_t> initial_release;
    if (_honor_waypoint_timing && _clock)
    {
        active.released_count = releasable_count(active.times, rmf_traffic_ros2::convert(_clock->now()));
        initial_release = active.released_count;
    }
    else
    {
        active.released_count = route.size();
    }

    // Continue the live order when the new route repeats its released part.
    if (_stitch_on_replan && had_active_path)
    {
        const auto replan = _connector.replan_route(_name, route, map_name, initial_release);
        if (replan.status == CommandStatus::transport_failed)
        {
            RCLCPP_ERROR(_logger,"[%s] follow_new_path: order update was not published (transport failure) -- replanning in %.0f s",  _name.c_str(), _route_policy.replan_after_s);
            schedule_replan();
            return;
        }
        if (replan.stitched)
        {
            // Drop the leading points the order already covers.
            if (replan.leading_dropped > 0)
            {
                const auto dropped = static_cast<std::ptrdiff_t>(replan.leading_dropped);
                active.node_ids.erase(active.node_ids.begin(), active.node_ids.begin() + dropped);
                active.positions.erase(active.positions.begin(), active.positions.begin() + dropped);
                active.times.erase(active.times.begin(), active.times.begin() + dropped);
                active.targets.erase(active.targets.begin(), active.targets.begin() + dropped);
                active.waypoint_offset += replan.leading_dropped;
            }
            active.order_id = replan.order_id;
            active.seq_offset = replan.consumed;
            active.released_count = replan.released;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _path = std::move(active);
                _replan_at.reset();
            }
            end_traffic_hold();
            return;
        }
    }

    bool stop_charging = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        stop_charging = _action_policy.charge_at_chargers && current->charging;
    }
    std::string stop_charging_id;
    if (stop_charging)
    {
        stop_charging_id = _connector.execute_instant_action(_name, "stopCharging");
        if (stop_charging_id.empty())
        {
            RCLCPP_WARN(_logger, "[%s] follow_new_path: stopCharging was not sent", _name.c_str());
        }
    }

    // Cancel the order this one replaces before sending a new orderId.
    if (_connector.order_in_progress(_name))
    {
        RCLCPP_INFO(_logger, "[%s] follow_new_path: %s -- cancelling it before publishing the replacement", _name.c_str(),  holding ? "replacing the held order" : "superseding an order still in progress");
        if (_connector.stop(_name) == CommandStatus::transport_failed)
        {
            RCLCPP_ERROR(_logger, "[%s] follow_new_path: cancelOrder was not published -- not sending the new order, replanning in %.0f s",  _name.c_str(), _route_policy.replan_after_s);
            schedule_replan();
            return;
        }
    }

    _connector.cancel_foreign_order(_name);
    if (_connector.cancel_pending(_name) || !charging_stopped(stop_charging_id))
    {
        RCLCPP_INFO(_logger, "[%s] follow_new_path: the new order waits for the AGV to answer cancelOrder / stopCharging", _name.c_str());
        std::lock_guard<std::mutex> lock(_mutex);
        _pending_order = PendingOrder{std::move(route), map_name, std::move(active), stop_charging_id};
        _replan_at.reset();
        return;
    }
    send_order(route, map_name, std::move(active));
}

void VdaRobotCommandHandle::send_order(const std::vector<Connector::RoutePoint> &route, const std::string &level, ActivePath active)
{
    std::optional<std::size_t> release;
    if (_honor_waypoint_timing && _clock)
    {
        active.released_count = releasable_count(active.times, rmf_traffic_ros2::convert(_clock->now()));
        release = active.released_count;
    }
    const auto result = _connector.navigate_route(_name, route, level, release);
    if (result.status != CommandStatus::queued)
    {
        RCLCPP_ERROR(_logger, "[%s] order was not published (%s) -- replanning in %.0f s", _name.c_str(),  result.status == CommandStatus::rejected ? "rejected by validation" : "transport failure",
                     _route_policy.replan_after_s);
        schedule_replan();
        return;
    }
    active.order_id = result.order_id;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _path = std::move(active);
        _replan_at.reset();
    }
    end_traffic_hold();
}

bool VdaRobotCommandHandle::charging_stopped(const std::string &action_id)
{
    if (action_id.empty())
    {
        return true;
    }
    const auto status = _connector.get_action_state(_name, action_id);
    if (status.has_value())
    {
        return vda5050::is_terminal_action_status(*status);
    }
    const auto data = _connector.get_data(_name);
    return data.has_value() && !data->charging;
}

void VdaRobotCommandHandle::send_pending_order()
{
    std::string stop_charging;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_pending_order.has_value())
        {
            return;
        }
        stop_charging = _pending_order->stop_charging;
    }
    if (_connector.cancel_pending(_name) || !charging_stopped(stop_charging))
    {
        return;
    }
    std::unique_lock<std::mutex> command(_command_mutex, std::try_to_lock);
    if (!command.owns_lock())
    {
        return;
    }
    std::optional<PendingOrder> pending;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        pending = std::exchange(_pending_order, std::nullopt);
    }
    if (!pending.has_value())
    {
        return;
    }
    RCLCPP_INFO(_logger, "[%s] cancelOrder / stopCharging settled -- sending the new order", _name.c_str());
    send_order(pending->route, pending->level, std::move(pending->path));
}

void VdaRobotCommandHandle::stop()
{
    std::lock_guard<std::mutex> command(_command_mutex);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_retired)
        {
            return;
        }
        if (_pending_order.has_value())
        {
            _pending_order.reset();
            RCLCPP_INFO(_logger, "[%s] stop: dropping the order that waited for cancelOrder", _name.c_str());
        }
    }
    // Hold only an order that may still run on the AGV.
    if (!_connector.order_in_progress(_name))
    {
        RCLCPP_DEBUG(_logger, "[%s] stop: no order in progress -- nothing to hold", _name.c_str());
        return;
    }

    // Pause on stop and cancel only if no replacement path arrives.
    if (_connector.paused_or_pausing(_name))
    {
        RCLCPP_DEBUG(_logger, "[%s] stop: the AGV is paused or pausing -- no second startPause", _name.c_str());
    }
    else if (_connector.pause(_name) == CommandStatus::transport_failed)
    {
        RCLCPP_ERROR(_logger,"[%s] stop: startPause did not reach the AGV (transport failure) -- ""the AGV may still be moving",_name.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    // A repeated stop keeps the hold's first deadline, so replans cannot postpone the cancellation forever.
    if (_hold != Hold::held)
    {
        _hold = Hold::held;
        _hold_deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(_route_policy.traffic_pause_timeout_s));
    }
    RCLCPP_INFO(_logger, "[%s] stop: held, awaiting a new path or the hold deadline", _name.c_str());
}

void VdaRobotCommandHandle::dock(const std::string &dock_name, RequestCompleted docking_finished_callback)
{
    DockAction action{dock_name, nlohmann::json::object()};
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_retired)
        {
            RCLCPP_WARN(_logger, "[%s] dock '%s': the robot was removed from the fleet -- ignoring it", _name.c_str(), dock_name.c_str());
            return;
        }
        _pending_order.reset();
        const auto mapped = _action_policy.dock_actions.find(dock_name);
        if (mapped != _action_policy.dock_actions.end())
        {
            action = mapped->second;
        }
    }
    const std::string action_id = _connector.execute_instant_action(_name, action.action_type, action.parameters);

    if (action_id.empty())
    {
        RCLCPP_ERROR(_logger,"[%s] dock '%s' was not published (unregistered robot, factsheet or transport "
                     "failure) -- replanning in %.0f s", _name.c_str(), dock_name.c_str(), _route_policy.replan_after_s);
        schedule_replan();
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    RCLCPP_INFO(_logger, "[%s] dock '%s' as '%s' (action %s)", _name.c_str(), dock_name.c_str(), action.action_type.c_str(), action_id.c_str());
    _dock_action_id = action_id;
    _dock_finished = std::move(docking_finished_callback);
}

void VdaRobotCommandHandle::on_perform_action(const std::string &category, const nlohmann::json &description, RobotUpdateHandle::ActionExecution execution)
{
    RCLCPP_INFO(_logger, "[%s] action '%s'", _name.c_str(), category.c_str());
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_retired)
        {
            execution.error("The robot was removed from the fleet");
            return;
        }
    }

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

void VdaRobotCommandHandle::report_position_again()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _idle_position.reset();
}

void VdaRobotCommandHandle::report_position(RobotUpdateHandle &handle, const RobotData &data)
{
    const Eigen::Vector3d position(data.position[0], data.position[1], data.position[2]);
    std::optional<RouteTarget> target;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_path.has_value() && !_path->targets.empty())
        {
            target = _path->targets[std::min(_path->next_index, _path->targets.size() - 1)];
            _idle_position.reset();
        }
    }

    const PositionUpdate report = _graph ? choose_position_update(*_graph, data.map_name, position, target, _route_policy.merge_waypoint_m, _route_policy.merge_lane_m)
                                         : PositionUpdate{};
    switch (report.kind)
    {
        case PositionUpdate::Kind::waypoint:
            handle.update_position(report.waypoint, position.z());
            return;
        case PositionUpdate::Kind::lanes:
            handle.update_position(position, report.lanes);
            return;
        case PositionUpdate::Kind::map:
            break;
    }

    if (!target.has_value())
    {
        // An idle robot that has not moved is not reported again.
        std::lock_guard<std::mutex> lock(_mutex);
        if (_idle_position.has_value() && _idle_position->first == data.map_name)
        {
            const Eigen::Vector3d &last = _idle_position->second;
            const double dtheta = std::atan2(std::sin(position.z() - last.z()), std::cos(position.z() - last.z()));
            if (std::hypot(position.x() - last.x(), position.y() - last.y()) < _route_policy.same_pose_m &&
                std::fabs(dtheta) < _route_policy.same_pose_rad)
            {
                return;
            }
        }
        _idle_position = std::make_pair(data.map_name, position);
    }
    handle.update_position(data.map_name, position);
}

void VdaRobotCommandHandle::update(const RobotData &data)
{
    // Read the update handle under the command lock.
    std::shared_ptr<RobotUpdateHandle> handle;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_retired)
        {
            return;
        }
        handle = _update_handle;
    }
    if (!handle)
    {
        return;
    }

    report_position(*handle, data);
    handle->update_battery_soc(data.battery_soc);

    // Follow a pause or resume the AGV reports that this handle did not request.
    std::optional<bool> reported_before;
    bool paused_here = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        reported_before = _agv_paused;
        _agv_paused = data.paused;
        paused_here = _paused;
    }
    if (reported_before != data.paused)
    {
        if (data.paused && !paused_here)
        {
            const auto saved_delay = handle->maximum_delay();
            handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());
            std::lock_guard<std::mutex> lock(_mutex);
            _saved_maximum_delay = saved_delay;
            _paused = true;
            RCLCPP_INFO(_logger, "[%s] AGV reports paused outside pause()/resume() -- syncing RMF", _name.c_str());
        }
        else if (!data.paused && paused_here && reported_before == true)
        {
            rmf_utils::optional<rmf_traffic::Duration> saved_delay;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                saved_delay = _saved_maximum_delay;
            }
            handle->maximum_delay(saved_delay);
            std::lock_guard<std::mutex> lock(_mutex);
            _paused = false;
            // An AGV that is no longer paused ends the operator's pause as well.
            _operator_paused = false;
            RCLCPP_INFO(_logger, "[%s] AGV reports not paused outside pause()/resume() -- syncing RMF", _name.c_str());
        }
    }

    expire_traffic_hold();
    send_pending_order();

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hold == Hold::released && (!data.paused || std::chrono::steady_clock::now() >= _hold_deadline))
        {
            _hold = Hold::none;
        }
        // A traffic-hold pause does not make the AGV unavailable.
        const bool traffic_hold = _hold != Hold::none && !_operator_paused;
        _ready_for_orders = data.ready_for_orders(traffic_hold);
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
        else if (data.paused && !traffic_hold)
        {
            _not_ready_reason = "AGV reports paused";
        }
        else
        {
            _not_ready_reason.clear();
        }
    }
    apply_commission();
    report_agv_errors(*handle, data);

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
    bool retry_replan = false;
    bool start_charging = false;
    std::string refused_order;
    std::string refused_reason;
    std::optional<RobotUpdateHandle::IssueTicket> accepted_issue;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_replan_at.has_value() && std::chrono::steady_clock::now() >= *_replan_at)
        {
            _replan_at.reset();
            retry_replan = true;
        }

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
                    action_exec = std::exchange(_action_exec, std::nullopt);
                    _action_id.clear();
                }
            }
        }

        if (_path.has_value())
        {
            if (const auto refusal = _connector.refusal_of(_name, _path->order_id))
            {
                refused_order = _path->order_id;
                refused_reason = *refusal;
                _path.reset();
            }
            else if (_order_issue.has_value() && data.order_id == _path->order_id)
            {
                accepted_issue = std::exchange(_order_issue, std::nullopt);
            }
        }

        if (_path.has_value())
        {
            auto &path = *_path;

            // Release route points on schedule, except while the AGV is held for a replan.
            if (_honor_waypoint_timing && _clock && _hold != Hold::held && path.released_count < path.times.size())
            {
                const auto now = rmf_traffic_ros2::convert(_clock->now());
                const std::size_t releasable = releasable_count(path.times, now);
                if (releasable > path.released_count &&
                    _connector.release_more(_name, path.order_id, releasable) == CommandStatus::queued)
                {
                    path.released_count = releasable;
                }
            }

            const Eigen::Vector3d here(data.position[0], data.position[1], data.position[2]);

            // Match sequence progress only to the active order ID.
            if (data.last_node_sequence_id.has_value() && !path.order_id.empty() &&
                data.order_id == path.order_id)
            {
                // Sequence IDs count from the start of the order, not of this path.
                const std::size_t reached = static_cast<std::size_t>(*data.last_node_sequence_id) / 2;
                const std::size_t passed = reached > path.seq_offset ? reached - path.seq_offset : 0;
                path.next_index = std::max(path.next_index, std::min(passed, path.node_ids.size()));
            }

            // Advance progress when the AGV reports or reaches a waypoint.
            while (path.next_index < path.node_ids.size())
            {
                const auto &target = path.positions[path.next_index];
                const bool reported =  !data.last_node_id.empty() && path.node_ids[path.next_index] == data.last_node_id;
                const bool standing_on_it = std::hypot(target.x() - here.x(), target.y() - here.y()) <= _route_policy.waypoint_reached_m;
                bool reached = reported || standing_on_it;

                // Keep repeated node IDs for in-place turns.
                if (reached && path.next_index > 0 && path.node_ids[path.next_index] == path.node_ids[path.next_index - 1])
                {
                    double dtheta = target.z() - here.z();
                    dtheta = std::atan2(std::sin(dtheta), std::cos(dtheta));
                    reached = std::fabs(dtheta) < _route_policy.same_pose_rad;
                }

                if (!reached)
                {
                    break;
                }

                // Report early arrival against RMF's schedule.
                if (path.next_index < path.times.size() && _clock)
                {
                    const double early = std::chrono::duration<double>( path.times[path.next_index] - rmf_traffic_ros2::convert(_clock->now())).count();

                    if (early > _route_policy.early_arrival_warn_s)
                    {
                        RCLCPP_WARN(_logger, "[%s] reached waypoint %zu ~%.1fs ahead of the time RMF " "planned around -- other itineraries assumed this robot ""would not be here yet",  _name.c_str(), path.next_index, early);
                    }
                }
                ++path.next_index;
            }

            if (_connector.is_command_completed(_name))
            {
                RCLCPP_INFO(_logger, "[%s] path completed", _name.c_str());
                if (_action_policy.charge_at_chargers && !data.charging && _graph && !path.targets.empty())
                {
                    const auto &end = path.targets.back().waypoint;
                    start_charging = end.has_value() && *end < _graph->num_waypoints() && _graph->get_waypoint(*end).is_charger();
                }
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
    if (accepted_issue.has_value())
    {
        accepted_issue->resolve({{"message", "the AGV accepted order " + data.order_id}});
    }
    if (!refused_order.empty())
    {
        RCLCPP_ERROR(_logger, "[%s] order '%s' refused by the AGV (%s) -- dropped, replanning in %.0f s", _name.c_str(),
                     refused_order.c_str(), refused_reason.c_str(), _route_policy.replan_after_s);
        bool raised = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            raised = _order_issue.has_value();
        }
        if (!raised)
        {
            auto ticket = handle->create_issue(RobotUpdateHandle::Tier::Error, "vda5050_order_refused",
                                               {{"order_id", refused_order}, {"error_type", refused_reason}});
            std::lock_guard<std::mutex> lock(_mutex);
            _order_issue.emplace(std::move(ticket));
        }
        schedule_replan();
    }
    if (start_charging)
    {
        RCLCPP_INFO(_logger, "[%s] at its charger -- startCharging", _name.c_str());
        if (_connector.execute_instant_action(_name, "startCharging").empty())
        {
            RCLCPP_WARN(_logger, "[%s] startCharging was not sent", _name.c_str());
        }
    }
    if (retry_replan)
    {
        RCLCPP_WARN(_logger, "[%s] the last command could not be sent -- asking RMF to replan", _name.c_str());
        handle->replan();
    }
    if (trigger_replan)
    {
        RCLCPP_WARN(_logger, "[%s] order unacknowledged for too long -- asking RMF to replan",_name.c_str());
        handle->replan();
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

void VdaRobotCommandHandle::set_update_handle(const std::shared_ptr<RobotUpdateHandle> &handle)
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
        handle->maximum_delay(timed_release_delay_limit());
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

void VdaRobotCommandHandle::set_action_policy(const ActionPolicy &policy)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _action_policy = policy;
}

std::optional<double> VdaRobotCommandHandle::edge_speed_limit(const rmf_traffic::agv::Plan::Waypoint &wp) const
{
    const auto limit = lane_speed_limit(wp);
    if (!_route_policy.cap_speed_to_fleet)
    {
        return limit;
    }
    return limit.has_value() ? std::min(*limit, _nominal_speed) : _nominal_speed;
}

bool VdaRobotCommandHandle::added() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return static_cast<bool>(_update_handle);
}

std::string VdaRobotCommandHandle::pause()
{
    std::shared_ptr<RobotUpdateHandle> handle;
    bool already_paused = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_update_handle)
        {
            return "robot is not in the RMF fleet yet";
        }
        if (_operator_paused)
        {
            return "already paused";
        }
        handle = _update_handle;
        already_paused = _paused;
    }

    // Pause without cancelling the order and lift the delay ceiling, unless a pause already lifted it.
    rmf_utils::optional<rmf_traffic::Duration> saved_delay;
    if (!already_paused)
    {
        saved_delay = handle->maximum_delay();
        handle->maximum_delay(rmf_utils::optional<rmf_traffic::Duration>());
    }

    const auto status = _connector.pause(_name);
    if (status == CommandStatus::transport_failed)
    {
        // Restore the delay ceiling if the pause action fails.
        if (!already_paused)
        {
            handle->maximum_delay(saved_delay);
        }
        RCLCPP_ERROR(_logger, "[%s] pause: startPause did not reach the AGV (transport failure)", _name.c_str());
        return "startPause failed to reach the AGV (MQTT publish failed)";
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!already_paused)
        {
            _saved_maximum_delay = saved_delay;
        }
        _paused = true;
        _operator_paused = true;
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
        if (_hold == Hold::held)
        {
            // RMF's traffic hold keeps the AGV paused; it is released with the next path.
            _operator_paused = false;
            RCLCPP_INFO(_logger, "[%s] operator pause lifted -- the AGV stays held until RMF sends a new path", _name.c_str());
            return {};
        }
        handle = _update_handle;
        saved_delay = _saved_maximum_delay;
    }

    // Resume the AGV before restoring the delay ceiling.
    const auto status = _connector.resume(_name);
    if (status == CommandStatus::transport_failed)
    {
        // Keep the pause state if stopPause fails.
        RCLCPP_ERROR(_logger,"[%s] resume: stopPause did not reach the AGV (transport failure) -- ""still paused",_name.c_str());
        return "stopPause failed to reach the AGV (MQTT publish failed) -- still paused";
    }

    if (handle)
    {
        handle->maximum_delay(saved_delay);
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _paused = false;
        _operator_paused = false;
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

void VdaRobotCommandHandle::expire_traffic_hold()
{
    const auto due = [this]()
    {
        return _hold == Hold::releasing || (_hold == Hold::held && !_pending_order.has_value() && std::chrono::steady_clock::now() >= _hold_deadline);
    };
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!due())
        {
            return;
        }
    }

    // A command in progress settles the hold itself; the next call retries.
    std::unique_lock<std::mutex> command(_command_mutex, std::try_to_lock);
    if (!command.owns_lock())
    {
        return;
    }
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!due())
        {
            return;
        }
        expired = _hold == Hold::held;
        if (expired)
        {
            _path.reset();
        }
    }
    if (expired)
    {
        RCLCPP_WARN(_logger, "[%s] stop: no new path within %.0f s -- cancelling the held order", _name.c_str(), _route_policy.traffic_pause_timeout_s);
        if (_connector.order_in_progress(_name) && _connector.stop(_name) == CommandStatus::transport_failed)
        {
            return;
        }
    }
    release_traffic_hold();
}

void VdaRobotCommandHandle::end_traffic_hold()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hold != Hold::held && _hold != Hold::releasing)
        {
            return;
        }
    }
    release_traffic_hold();
}

void VdaRobotCommandHandle::release_traffic_hold()
{
    bool operator_paused = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        operator_paused = _operator_paused;
    }
    if (!operator_paused && !_connector.pause_settled(_name))
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _hold = Hold::releasing;
        return;
    }
    const bool failed = !operator_paused && _connector.stop_pause_needed(_name) && _connector.resume(_name) == CommandStatus::transport_failed;
    std::lock_guard<std::mutex> lock(_mutex);
    if (operator_paused)
    {
        _hold = Hold::none;
    }
    else if (failed)
    {
        _hold = Hold::releasing;
    }
    else
    {
        _hold = Hold::released;
        _hold_deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                               std::chrono::duration<double>(_route_policy.traffic_pause_timeout_s));
    }
}

void VdaRobotCommandHandle::retire()
{
    std::unique_lock<std::mutex> command(_command_mutex);
    std::optional<RobotUpdateHandle::ActionExecution> action;
    bool held = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _retired = true;
        _path.reset();
        _replan_at.reset();
        _dock_action_id.clear();
        _dock_finished = nullptr;
        action = std::exchange(_action_exec, std::nullopt);
        _action_id.clear();
        held = _hold == Hold::held || _hold == Hold::releasing;
        _pending_order.reset();
        _hold = Hold::none;
        _ready_for_orders = false;
        _not_ready_reason = "removed by the operator";
    }
    apply_commission();

    // Nothing manages the robot from here on, so it must not carry on with an order.
    if (_connector.order_in_progress(_name) && _connector.stop(_name) == CommandStatus::transport_failed)
    {
        RCLCPP_WARN(_logger, "[%s] cancelOrder did not reach the AGV; it may finish its last order", _name.c_str());
    }
    if (held)
    {
        release_traffic_hold();
        std::lock_guard<std::mutex> lock(_mutex);
        _hold = Hold::none;
    }
    command.unlock();
    if (action.has_value())
    {
        action->error("The robot was removed from the fleet");
    }
}

void VdaRobotCommandHandle::restore()
{
    std::shared_ptr<RobotUpdateHandle> handle;
    {
        std::lock_guard<std::mutex> command(_command_mutex);
        std::lock_guard<std::mutex> lock(_mutex);
        _retired = false;
        handle = _update_handle;
    }
    // Ask RMF for a new plan; readiness is checked again with the next state.
    if (handle)
    {
        handle->replan();
    }
}

void VdaRobotCommandHandle::report_agv_errors(RobotUpdateHandle &handle, const RobotData &data)
{
    std::map<std::string, const vda5050::AgvError *> reported;
    for (const auto &error : data.errors)
    {
        reported.emplace(error.level + "/" + error.type, &error);
    }

    std::vector<const vda5050::AgvError *> raised;
    std::vector<std::pair<std::string, RobotUpdateHandle::IssueTicket>> cleared;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto it = _agv_error_issues.begin(); it != _agv_error_issues.end();)
        {
            if (reported.count(it->first) == 0)
            {
                cleared.emplace_back(it->first, std::move(it->second));
                it = _agv_error_issues.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (const auto &[key, error] : reported)
        {
            if (_agv_error_issues.count(key) == 0)
            {
                raised.push_back(error);
            }
        }
    }

    // Create and resolve issues outside the command lock.
    for (auto &[key, ticket] : cleared)
    {
        RCLCPP_INFO(_logger, "[%s] AGV error '%s' cleared", _name.c_str(), key.c_str());
        ticket.resolve({{"message", "the AGV no longer reports this error"}});
    }
    for (const auto *error : raised)
    {
        const auto tier = error->level == "FATAL" ? RobotUpdateHandle::Tier::Error : RobotUpdateHandle::Tier::Warning;
        RCLCPP_WARN(_logger, "[%s] AGV reports %s error '%s': %s", _name.c_str(), error->level.c_str(), error->type.c_str(),
                    error->description.c_str());
        auto ticket = handle.create_issue(tier, "vda5050_agv_error",
                                          {{"error_type", error->type}, {"error_level", error->level}, {"description", error->description}});
        std::lock_guard<std::mutex> lock(_mutex);
        _agv_error_issues.emplace(error->level + "/" + error->type, std::move(ticket));
    }
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
        commission = !_retired && _online.value_or(false) && _ready_for_orders;
        if (_commissioned == commission)
        {
            return;
        }
        _commissioned = commission;
        handle = _update_handle;
        reason = _retired ? "removed by the operator" : !_online.value_or(false) ? "no recent VDA5050 state" : _not_ready_reason;
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
