#ifndef ROBOT_COMMAND_HANDLE_HPP
#define ROBOT_COMMAND_HANDLE_HPP

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rclcpp/clock.hpp>
#include <rclcpp/logger.hpp>
#include <rmf_fleet_adapter/agv/RobotCommandHandle.hpp>
#include <rmf_fleet_adapter/agv/RobotUpdateHandle.hpp>
#include <rmf_traffic/agv/Graph.hpp>
#include <rmf_traffic/agv/Planner.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/action_policy.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/position_update.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/route_policy.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

// Implement RMF FullControl commands as VDA5050 multi-node orders.
class VdaRobotCommandHandle : public rmf_fleet_adapter::agv::RobotCommandHandle,
    public std::enable_shared_from_this<VdaRobotCommandHandle>
{
public:
    using Base = rmf_fleet_adapter::agv::RobotCommandHandle;
    using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;
    using ArrivalEstimator = Base::ArrivalEstimator;
    using RequestCompleted = Base::RequestCompleted;

    // The connector and graph must outlive this handle; the clock uses the RMF plan's time source.
    VdaRobotCommandHandle(const rclcpp::Logger &logger, std::string name,  Connector &connector, std::shared_ptr<const rmf_traffic::agv::Graph> graph,
                          double nominal_speed, rclcpp::Clock::SharedPtr clock,
                          bool honor_waypoint_timing = false,  bool stitch_on_replan = false, const RoutePolicy &route_policy = {});

    // RMF RobotCommandHandle interface.
    void follow_new_path(const std::vector<rmf_traffic::agv::Plan::Waypoint> &waypoints, ArrivalEstimator next_arrival_estimator, RequestCompleted path_finished_callback) override;

    void stop() override;

    void dock(const std::string &dock_name, RequestCompleted docking_finished_callback) override;

    // Dispatches an RMF PerformAction activity as a VDA5050 instant action.
    void on_perform_action(const std::string &category, const nlohmann::json &description, RobotUpdateHandle::ActionExecution execution);

    // Publishes robot state to RMF and advances active command tracking.
    void update(const RobotData &data);

    void set_update_handle(const std::shared_ptr<RobotUpdateHandle> &handle);

    // Set dock and charging actions.
    void set_action_policy(const ActionPolicy &policy);
    bool added() const;

    // Updates RMF commission state from VDA5050 connectivity and readiness.
    void set_online(bool online);

    // Update readiness when no usable pose is available to the regular update loop.
    void set_ready_for_orders(bool ready, const std::string &reason = "");

    // Ends a traffic hold that got no new path in time: cancels the held order and releases the pause, retrying a stopPause that failed.
    void expire_traffic_hold();

    // Sends the order held back for a cancelOrder once the AGV has answered it.
    void send_pending_order();

    // Reports the position to RMF on the next update even if the robot has not moved, e.g. after lane closures change.
    void report_position_again();

    // Stops managing the robot: drops its command, cancels its order and decommissions it.
    void retire();

    // Manages a retired robot again and asks RMF for a new plan.
    void restore();

    // Pause the AGV without clearing its order; return an error string on failure.
    std::string pause();

    // Resume the AGV's paused order; return an error string on failure.
    std::string resume();

    // Derives a stable VDA5050 nodeId from RMF waypoint metadata.
    static std::string derive_node_id(const std::string &name, std::optional<std::size_t> graph_index, double x, double y);

private:
    // Track progress through the active RMF path, including any skipped leading waypoint.
    struct ActivePath
    {
        // VDA5050 orderId associated with this path.
        std::string order_id;
        std::vector<std::string> node_ids;  // one per RMF path index
        std::vector<Eigen::Vector3d> positions;
        // Planned arrival times used to report schedule drift.
        std::vector<rmf_traffic::Time> times;
        std::size_t next_index = 0;
        // Count of leading RMF waypoints that duplicate the AGV's current pose.
        std::size_t waypoint_offset = 0;
        // Number of route points released in the latest order update.
        std::size_t released_count = 0;
        // Route points passed before this path began.
        std::size_t seq_offset = 0;
        // RMF's waypoint index and approach lanes for each route point.
        std::vector<RouteTarget> targets;
        // Whether this path has already requested a replan for a stuck order.
        bool replan_requested = false;
        ArrivalEstimator arrival_estimator;
        RequestCompleted finished;
    };

    // Look up the nav graph's name for a plan waypoint, if it has one.
    std::string node_id_for(const rmf_traffic::agv::Plan::Waypoint &wp) const;

    // Returns the most restrictive approach-lane speed limit for a waypoint.
    std::optional<double> lane_speed_limit(const rmf_traffic::agv::Plan::Waypoint &wp) const;

    // maxSpeed of the edge into a waypoint.
    std::optional<double> edge_speed_limit(const rmf_traffic::agv::Plan::Waypoint &wp) const;

    // Estimates travel time from the measured or nominal linear speed.
    double estimate_seconds(const Eigen::Vector3d &from, const Eigen::Vector3d &to, const std::optional<vda5050::Velocity> &velocity) const;
    
    // Applies the current commission decision to RMF when it changes.
    void apply_commission();

    // Opens an RMF issue for each error the AGV reports, and resolves it once the AGV clears the error.
    void report_agv_errors(RobotUpdateHandle &handle, const RobotData &data);

    // Sends the stopPause that ends a traffic hold, once the AGV has answered the startPause and unless the operator paused it.
    void release_traffic_hold();

    // Ends a traffic hold once the robot has a new order, then releases the pause.
    void end_traffic_hold();

    // Asks RMF for a new plan after replan_after_s, for a command that could not be sent.
    void schedule_replan();

    // Whether the stopCharging `action_id` has ended, or the AGV no longer reports charging without reporting the action.
    bool charging_stopped(const std::string &action_id);

    // Publishes a new order for `active` and makes it the active path.
    void send_order(const std::vector<Connector::RoutePoint> &route, const std::string &level, ActivePath active);

    // Reports the position to RMF with the waypoint or lanes of the active path when the robot is on them.
    void report_position(RobotUpdateHandle &handle, const RobotData &data);

    // RMF's delay limit while waypoint timing is honored.
    rmf_utils::optional<rmf_traffic::Duration> timed_release_delay_limit() const;

    rclcpp::Logger _logger;
    std::string _name;
    Connector &_connector;
    std::shared_ptr<const rmf_traffic::agv::Graph> _graph;
    double _nominal_speed;
    rclcpp::Clock::SharedPtr _clock;
    bool _honor_waypoint_timing;
    bool _stitch_on_replan;
    RoutePolicy _route_policy;
    ActionPolicy _action_policy;

    // Serializes the commands that change the robot's order: new paths, stops, hold expiry, retire and restore.
    std::mutex _command_mutex;
    // Protects command state shared by RMF and the update loop; taken after _command_mutex.
    mutable std::mutex _mutex;
    std::optional<ActivePath> _path;

    // A new order waiting for the AGV to answer the cancelOrder of the order it replaces, and its stopCharging.
    struct PendingOrder
    {
        std::vector<Connector::RoutePoint> route;
        std::string level;
        ActivePath path;
        // actionId of the stopCharging sent before it; empty when none.
        std::string stop_charging;
    };
    std::optional<PendingOrder> _pending_order;
    std::string _dock_action_id;
    RequestCompleted _dock_finished;

    // Active PerformAction state, independent of docking state.
    std::string _action_id;
    std::optional<RobotUpdateHandle::ActionExecution> _action_exec;

    std::shared_ptr<RobotUpdateHandle> _update_handle;
    // Whether VDA5050 state is current; nullopt before the first update.
    std::optional<bool> _online;
    // Whether the latest AGV state permits master-control orders.
    bool _ready_for_orders = true;
    // Diagnostic reason for the current readiness state.
    std::string _not_ready_reason;
    // Last commission state applied to RMF.
    std::optional<bool> _commissioned;
    // Operator pause state.
    bool _paused = false;
    // Set only by the operator's pause() and cleared by resume().
    bool _operator_paused = false;
    // Pause state the AGV last reported; nullopt before its first report.
    std::optional<bool> _agv_paused;
    // RMF's delay ceiling as it was before a pause lifted it.
    rmf_utils::optional<rmf_traffic::Duration> _saved_maximum_delay;
    // Traffic hold: the AGV is paused for RMF (held); once it has a new order or the hold expired, stopPause is due
    // (releasing), then sent and awaited until the AGV reports it is not paused (released).
    enum class Hold
    {
        none,
        held,
        releasing,
        released,
    };
    Hold _hold = Hold::none;
    // Deadline of the hold: when a held order is cancelled, or when a released AGV counts as unpaused.
    std::chrono::steady_clock::time_point _hold_deadline{};
    // When to ask RMF for a new plan after a command could not be sent.
    std::optional<std::chrono::steady_clock::time_point> _replan_at;
    // Removed from the fleet: no commands are carried out.
    bool _retired = false;
    // RMF issue for a refused order.
    std::optional<RobotUpdateHandle::IssueTicket> _order_issue;
    // Open RMF issues for AGV errors, keyed by "<level>/<errorType>".
    std::map<std::string, RobotUpdateHandle::IssueTicket> _agv_error_issues;
    // Map and pose last reported to RMF while no path was active.
    std::optional<std::pair<std::string, Eigen::Vector3d>> _idle_position;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // ROBOT_COMMAND_HANDLE_HPP
