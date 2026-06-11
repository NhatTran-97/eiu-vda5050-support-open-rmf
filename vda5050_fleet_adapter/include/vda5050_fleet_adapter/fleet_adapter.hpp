#pragma once

#include <vda5050_fleet_adapter/robot_command_handle.hpp>

#include <rmf_fleet_adapter/agv/Adapter.hpp>
#include <rmf_fleet_adapter/agv/FleetUpdateHandle.hpp>
#include <rmf_traffic/agv/VehicleTraits.hpp>
#include <rmf_traffic/agv/Graph.hpp>

#include <rclcpp/rclcpp.hpp>
#include <string>
#include <memory>

namespace vda5050_fleet_adapter {

struct FleetConfig
{
  std::string fleet_name;
  std::string nav_graph_path;
  std::string rmf_server_uri;   

  std::string mqtt_broker_url;      
  std::string interface_name;        
  std::string manufacturer;
  std::string serial_number;
  std::string robot_start_waypoint;  

  // VDA5050 map name reported in order nodePositions. MUST match both the level
  // name in nav_graph.yaml and the map_id the robot reports in agvPosition,
  // otherwise RMF's update_position() rejects the pose.
  std::string map_name = "tb3_world";

  // Vehicle traits
  double linear_velocity   = 0.5;    // m/s
  double angular_velocity  = 1.0;    // rad/s
  double linear_accel      = 0.3;    // m/s²
  double angular_accel     = 0.5;    // rad/s²
  double footprint_radius  = 0.3;    // m

  
  double battery_voltage          = 11.1;  // nominal voltage (V)
  double battery_capacity         = 1.8;   // capacity (Ah)
  double battery_charging_current = 0.5;   // charging current (A)
  double mass                     = 1.5;   // kg
  double moment_of_inertia        = 0.01;  // kg.m²
  double friction_coefficient     = 0.1;
  double ambient_power_draw       = 5.0;   // W (idle electronics)
  double tool_power_draw          = 0.0;   // W (payload tools, if any)
  double recharge_threshold       = 0.2;   // SOC at which a charger task triggers
  double recharge_soc             = 1.0;   // SOC to charge back up to
};

class FleetAdapter
{
public:
  explicit FleetAdapter(const FleetConfig& config);

  void run();

private:
  FleetConfig _config;
  std::shared_ptr<rmf_fleet_adapter::agv::Adapter> _adapter;
  std::shared_ptr<rmf_fleet_adapter::agv::FleetUpdateHandle> _fleet;
  std::shared_ptr<RobotCommandHandle> _robot_cmd;
  rmf_traffic::agv::Graph _fleet_graph;

  void _setup_fleet();
  void _add_robot();
  rmf_traffic::agv::VehicleTraits _make_traits() const;
  rmf_traffic::agv::Graph _build_graph() const;
  rmf_traffic::agv::Graph _load_graph_from_yaml(const std::string& path) const;
};

} // namespace vda5050_fleet_adapter
