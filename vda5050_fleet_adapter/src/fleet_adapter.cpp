#include <vda5050_fleet_adapter/fleet_adapter.hpp>

#include <rmf_fleet_adapter/agv/parse_graph.hpp>
#include <rmf_battery/agv/BatterySystem.hpp>
#include <rmf_battery/agv/MechanicalSystem.hpp>
#include <rmf_battery/agv/PowerSystem.hpp>
#include <rmf_battery/agv/SimpleMotionPowerSink.hpp>
#include <rmf_battery/agv/SimpleDevicePowerSink.hpp>
#include <rmf_traffic/Profile.hpp>
#include <rmf_traffic/geometry/Circle.hpp>
#include <rmf_traffic/geometry/ConvexShape.hpp>
#include <rmf_traffic/agv/VehicleTraits.hpp>
#include <rmf_traffic/agv/Graph.hpp>
#include <rmf_traffic_ros2/Time.hpp>

#include <yaml-cpp/yaml.h>
#include <rclcpp/rclcpp.hpp>
#include <thread>

namespace vda5050_fleet_adapter 
{

FleetAdapter::FleetAdapter(const FleetConfig& config): _config(config)
{
  _adapter = rmf_fleet_adapter::agv::Adapter::init_and_make(
    _config.fleet_name + "_fleet_adapter");

  _setup_fleet();
  _add_robot();
}

void FleetAdapter::run()
{
  RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
    "VDA5050 Fleet Adapter running...");
  _adapter->start();

  // Adapter manages its own node — just wait for shutdown signal
  while (rclcpp::ok()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  _adapter->stop();
  RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name), "Shutting down cleanly");
}

void FleetAdapter::_setup_fleet()
{
  const auto traits = _make_traits();

  // Load graph from nav_graph.yaml or fallback to hardcoded
  rmf_traffic::agv::Graph graph;
  if (!_config.nav_graph_path.empty()) {
    try {
      graph = _load_graph_from_yaml(_config.nav_graph_path);
      RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
        "Loaded graph from %s (%zu waypoints)",
        _config.nav_graph_path.c_str(), graph.num_waypoints());
    } catch (const std::exception& e) {
      RCLCPP_WARN(rclcpp::get_logger(_config.fleet_name),
        "Failed to load graph (%s), using hardcoded graph", e.what());
      graph = _build_graph();
    }
  } else {
    graph = _build_graph();
  }

  // Pass nullopt if server_uri is empty to disable BroadcastClient
  std::optional<std::string> server_uri;
  if (!_config.rmf_server_uri.empty())
    server_uri = _config.rmf_server_uri;

  _fleet_graph = graph;

  _fleet = _adapter->add_fleet(
    _config.fleet_name,
    traits,
    graph,
    server_uri);

  RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
    "Fleet [%s] added with %zu waypoints",
    _config.fleet_name.c_str(), graph.num_waypoints());

  // Battery + mechanical model — all parameters come from config so the adapter
  // carries no vendor-specific constants (defaults describe a TurtleBot3 Burger).
  const auto battery_sys = rmf_battery::agv::BatterySystem::make(
    _config.battery_voltage,
    _config.battery_capacity,
    _config.battery_charging_current);

  const auto mech_sys = rmf_battery::agv::MechanicalSystem::make(
    _config.mass,
    _config.moment_of_inertia,
    _config.friction_coefficient);

  const auto motion_sink =
    std::make_shared<rmf_battery::agv::SimpleMotionPowerSink>(
      *battery_sys, *mech_sys);

  const auto ambient_sink =
    std::make_shared<rmf_battery::agv::SimpleDevicePowerSink>(
      *battery_sys,
      *rmf_battery::agv::PowerSystem::make(_config.ambient_power_draw));

  const auto tool_sink =
    std::make_shared<rmf_battery::agv::SimpleDevicePowerSink>(
      *battery_sys,
      *rmf_battery::agv::PowerSystem::make(_config.tool_power_draw));

  const bool ok = _fleet->set_task_planner_params(
    std::make_shared<rmf_battery::agv::BatterySystem>(*battery_sys),
    motion_sink,
    ambient_sink,
    tool_sink,
    _config.recharge_threshold,
    _config.recharge_soc,
    true);  // account for battery drain → enables automatic charger task

  if (ok)
    RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
      "Task planner params set successfully");
  else
    RCLCPP_WARN(rclcpp::get_logger(_config.fleet_name),
      "Failed to set task planner params");
}

void FleetAdapter::_add_robot()
{
  _robot_cmd = std::make_shared<RobotCommandHandle>(
    _config.fleet_name + "_robot",
    _config.manufacturer,
    _config.serial_number,
    _config.mqtt_broker_url,
    _config.interface_name,
    _config.map_name);

  const auto footprint = rmf_traffic::geometry::make_final_convex<
    rmf_traffic::geometry::Circle>(_config.footprint_radius);

  const rmf_traffic::Profile profile{footprint};

  // Find start waypoint index by name from graph
  std::size_t start_wp_idx = 0;
  const auto& keys = _fleet_graph.keys();
  auto it = keys.find(_config.robot_start_waypoint);
  if (it != keys.end()) {
    start_wp_idx = it->second;
    RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
      "Robot start waypoint: %s (index %zu)",
      _config.robot_start_waypoint.c_str(), start_wp_idx);
  } else {
    RCLCPP_WARN(rclcpp::get_logger(_config.fleet_name),
      "Waypoint '%s' not found, defaulting to index 0",
      _config.robot_start_waypoint.c_str());
  }

  const rmf_traffic::agv::Plan::Start start{
    rmf_traffic_ros2::convert(_adapter->node()->now()), start_wp_idx, 0.0};

  // Find the first charger waypoint in the graph to assign as dedicated charger
  std::optional<std::size_t> charger_wp_idx;
  for (std::size_t i = 0; i < _fleet_graph.num_waypoints(); ++i) {
    if (_fleet_graph.get_waypoint(i).is_charger()) {
      charger_wp_idx = i;
      break;
    }
  }

  if (!charger_wp_idx.has_value()) {
    RCLCPP_WARN(rclcpp::get_logger(_config.fleet_name),
      "No charger waypoint found in graph — battery charging will not work");
  } else {
    RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
      "Designated charger waypoint: index %zu", charger_wp_idx.value());
  }

  _fleet->add_robot(
    _robot_cmd,
    _config.fleet_name + "_robot",
    profile,
    {start},
    [this, charger_wp_idx](std::shared_ptr<rmf_fleet_adapter::agv::RobotUpdateHandle> updater) {
      if (charger_wp_idx.has_value())
        updater->set_charger_waypoint(charger_wp_idx.value());
      updater->update_battery_soc(1.0);  // start at 100%; charger task triggers when battery drops below 20%
      _robot_cmd->set_updater(updater);
      RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
        "Robot [%s] added and ready",
        (_config.fleet_name + "_robot").c_str());
    });
}

rmf_traffic::agv::Graph FleetAdapter::_build_graph() const
{
  // Simple test graph — 6 waypoints, 5 lanes (bidirectional)
  //
  //  start(0) -- wp1(1) -- wp2(2) -- wp3(3) -- goal_1(4)
  //                                         \-- goal_2(5)
  //
  // Coordinates in meters (match nav_graph.yaml)
  const std::string map = "L1";

  rmf_traffic::agv::Graph graph;

  // Waypoints: (x, y)
  graph.add_waypoint(map, {0.0,  0.0}).set_charger(true).set_holding_point(true);  // 0: start/charger
  graph.add_waypoint(map, {2.0,  0.0});   // 1: wp_1
  graph.add_waypoint(map, {4.0,  0.0});   // 2: wp_2
  graph.add_waypoint(map, {6.0,  0.0});   // 3: wp_3
  graph.add_waypoint(map, {6.0,  2.0}).set_holding_point(true);   // 4: goal_1
  graph.add_waypoint(map, {6.0, -2.0}).set_holding_point(true);   // 5: goal_2

  // Name key → waypoint index (for task dispatch)
  graph.add_key("start",  0);
  graph.add_key("wp_1",   1);
  graph.add_key("wp_2",   2);
  graph.add_key("wp_3",   3);
  graph.add_key("goal_1", 4);
  graph.add_key("goal_2", 5);

  using Node = rmf_traffic::agv::Graph::Lane::Node;

  // Bidirectional lanes (add both directions)
  auto bidir = [&](std::size_t a, std::size_t b) {
    graph.add_lane(Node(a), Node(b));
    graph.add_lane(Node(b), Node(a));
  };
  bidir(0, 1);
  bidir(1, 2);
  bidir(2, 3);
  bidir(3, 4);
  bidir(3, 5);

  return graph;
}

rmf_traffic::agv::VehicleTraits FleetAdapter::_make_traits() const
{
  const auto footprint = rmf_traffic::geometry::make_final_convex<
    rmf_traffic::geometry::Circle>(_config.footprint_radius);

  return rmf_traffic::agv::VehicleTraits{
    {_config.linear_velocity,  _config.linear_accel},
    {_config.angular_velocity, _config.angular_accel},
    rmf_traffic::Profile{footprint}
  };
}

rmf_traffic::agv::Graph FleetAdapter::_load_graph_from_yaml(
  const std::string& path) const
{
  const auto yaml = YAML::LoadFile(path);
  const auto levels = yaml["levels"];
  if (!levels) throw std::runtime_error("No 'levels' key in " + path);

  // Use first level found
  const auto level_it = levels.begin();
  const std::string map_name = level_it->first.as<std::string>();
  const auto level = level_it->second;

  rmf_traffic::agv::Graph graph;
  using Node = rmf_traffic::agv::Graph::Lane::Node;

  // Load vertices
  for (const auto& v : level["vertices"]) {
    const double x = v[0].as<double>();
    const double y = v[1].as<double>();
    auto& wp = graph.add_waypoint(map_name, {x, y});

    // Read optional properties
    if (v.size() > 2 && v[2].IsMap()) {
      const auto& props = v[2];
      if (props["name"])
        graph.add_key(props["name"].as<std::string>(), graph.num_waypoints() - 1);
      if (props["is_charger"] && props["is_charger"].as<bool>())
        wp.set_charger(true);
      if (props["is_parking_spot"] && props["is_parking_spot"].as<bool>())
        wp.set_holding_point(true);
    }
  }

  // Load lanes
  for (const auto& l : level["lanes"]) {
    const std::size_t from = l[0].as<std::size_t>();
    const std::size_t to   = l[1].as<std::size_t>();
    graph.add_lane(Node(from), Node(to));
  }

  RCLCPP_INFO(rclcpp::get_logger(_config.fleet_name),
    "Graph loaded: map=%s, waypoints=%zu, lanes=%zu",
    map_name.c_str(), graph.num_waypoints(), graph.num_lanes());

  return graph;
}

} // namespace vda5050_fleet_adapter
