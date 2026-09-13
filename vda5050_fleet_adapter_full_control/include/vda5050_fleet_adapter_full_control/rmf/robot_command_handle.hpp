#ifndef ROBOT_COMMAND_HANDLE_HPP
#define ROBOT_COMMAND_HANDLE_HPP

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

namespace vda5050_fleet_adapter_full_control::rmf {

// Implements RMF FullControl commands for one VDA5050 robot. Planned routes
// are published as multi-node VDA5050 orders.
class VdaRobotCommandHandle
  : public rmf_fleet_adapter::agv::RobotCommandHandle,
    public std::enable_shared_from_this<VdaRobotCommandHandle>
{
public:
    using Base = rmf_fleet_adapter::agv::RobotCommandHandle;
    using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;
    using ArrivalEstimator = Base::ArrivalEstimator;
    using RequestCompleted = Base::RequestCompleted;

    // `connector` and `graph` must outlive this object; `clock` must share
    // the RMF plan's time source -- see Config::honor_waypoint_timing().
    VdaRobotCommandHandle(rclcpp::Logger logger, std::string name,
                          Connector &connector,
                          std::shared_ptr<const rmf_traffic::agv::Graph> graph,
                          double nominal_speed,
                          rclcpp::Clock::SharedPtr clock,
                          bool honor_waypoint_timing = false);

    // rmf_fleet_adapter::agv::RobotCommandHandle
    void follow_new_path(
        const std::vector<rmf_traffic::agv::Plan::Waypoint> &waypoints,
        ArrivalEstimator next_arrival_estimator,
        RequestCompleted path_finished_callback) override;

    void stop() override;

    void dock(const std::string &dock_name,
              RequestCompleted docking_finished_callback) override;

    // Dispatches an RMF PerformAction activity as a VDA5050 instant action.
    void on_perform_action(const std::string &category, const nlohmann::json &description,
                           RobotUpdateHandle::ActionExecution execution);

    // Publishes robot state to RMF and advances active command tracking.
    void update(const RobotData &data);

    void set_update_handle(std::shared_ptr<RobotUpdateHandle> handle);
    bool added() const;

    // Updates RMF commission state from VDA5050 connectivity and readiness.
    void set_online(bool online);

    // Marks readiness outside of update() -- e.g. a lost pose, which update()
    // itself can't catch since it only runs when a pose is available.
    void set_ready_for_orders(bool ready, const std::string &reason = "");

    // Pauses the AGV while preserving its active order. Returns an empty
    // string on success or an error description on failure.
    std::string pause();

    // Resumes a paused order. Uses the same result convention as pause().
    std::string resume();

    // Derives a stable VDA5050 nodeId from RMF waypoint metadata.
    static std::string derive_node_id(const std::string &name, std::optional<std::size_t> graph_index, double x, double y);

private:
    // Progress state for the active RMF path. May be indexed from RMF waypoints[1], not [0] -- see follow_new_path(); waypoint_offset records which, so ArrivalEstimator's path_index can add it back.
    struct ActivePath
    {
        // VDA5050 orderId associated with this path.
        std::string order_id;
        std::vector<std::string> node_ids;  // one per RMF path index
        std::vector<Eigen::Vector3d> positions;
        // Planned arrival times used for schedule diagnostics. The current VDA5050 order does not encode waypoint hold times.
        std::vector<rmf_traffic::Time> times;
        std::size_t next_index = 0;
        // 0 or 1: how many leading RMF waypoints were dropped as redundant with the AGV's current pose before this path was built.
        std::size_t waypoint_offset = 0;
        // How many leading route points are released, as last published -- meaningful only when honor_waypoint_timing() is on. Mirrors Connector::RobotContext::current_released_count.
        std::size_t released_count = 0;
        // Set once a stuck-order timeout has already triggered a replan for this path, so a stuck order is reported and replanned once, not every update() tick until it resolves.
        bool replan_requested = false;
        ArrivalEstimator arrival_estimator;
        RequestCompleted finished;
    };

    // Look up the nav graph's name for a plan waypoint, if it has one.
    std::string node_id_for(const rmf_traffic::agv::Plan::Waypoint &wp) const;

    // Returns the most restrictive approach-lane speed limit for a waypoint.
    std::optional<double> lane_speed_limit(const rmf_traffic::agv::Plan::Waypoint &wp) const;

    // Estimates travel time from the measured or nominal linear speed.
    double estimate_seconds(const Eigen::Vector3d &from, const Eigen::Vector3d &to, const std::optional<vda5050::Velocity> &velocity) const;
    
    // Applies the current commission decision to RMF when it changes.
    void apply_commission();

    rclcpp::Logger _logger;
    std::string _name;
    Connector &_connector;
    std::shared_ptr<const rmf_traffic::agv::Graph> _graph;
    double _nominal_speed;
    rclcpp::Clock::SharedPtr _clock;
    bool _honor_waypoint_timing;

    // Protects command state shared by RMF and the update loop.
    mutable std::mutex _mutex;
    std::optional<ActivePath> _path;
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
    // RMF's delay ceiling as it was before a pause lifted it.
    rmf_utils::optional<rmf_traffic::Duration> _saved_maximum_delay;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // ROBOT_COMMAND_HANDLE_HPP
