#include <vda5050_fleet_adapter/fleet_adapter.hpp>
#include <rclcpp/rclcpp.hpp>

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);

  auto param_node = std::make_shared<rclcpp::Node>(
    "vda5050_fleet_adapter_params",
    rclcpp::NodeOptions().allow_undeclared_parameters(true));

  vda5050_fleet_adapter::FleetConfig config;

  config.fleet_name     = param_node->declare_parameter("fleet_name",     "tb3_fleet");
  config.nav_graph_path = param_node->declare_parameter("nav_graph_path", "");
  config.rmf_server_uri = param_node->declare_parameter("rmf_server_uri", "ws://localhost:7878");
  config.mqtt_broker_url = param_node->declare_parameter("mqtt_broker_url", "tcp://localhost:1883");
  config.interface_name         = param_node->declare_parameter("interface_name",         "uagv");
  config.manufacturer           = param_node->declare_parameter("manufacturer",           "ROBOTIS");
  config.serial_number          = param_node->declare_parameter("serial_number",          "0001");
  config.robot_start_waypoint   = param_node->declare_parameter("robot_start_waypoint",   "initial_wp");
  config.map_name               = param_node->declare_parameter("map_name",               "tb3_world");

  config.linear_velocity  = param_node->declare_parameter("linear_velocity",  0.22);
  config.angular_velocity = param_node->declare_parameter("angular_velocity", 1.82);
  config.linear_accel     = param_node->declare_parameter("linear_accel",     0.5);
  config.angular_accel    = param_node->declare_parameter("angular_accel",    1.0);
  config.footprint_radius = param_node->declare_parameter("footprint_radius", 0.105);

  // Battery / power model — vendor-specific, defaults describe a TurtleBot3 Burger.
  config.battery_voltage          = param_node->declare_parameter("battery_voltage",          11.1);
  config.battery_capacity         = param_node->declare_parameter("battery_capacity",         1.8);
  config.battery_charging_current = param_node->declare_parameter("battery_charging_current", 0.5);
  config.mass                     = param_node->declare_parameter("mass",                     1.5);
  config.moment_of_inertia        = param_node->declare_parameter("moment_of_inertia",        0.01);
  config.friction_coefficient     = param_node->declare_parameter("friction_coefficient",     0.1);
  config.ambient_power_draw       = param_node->declare_parameter("ambient_power_draw",       5.0);
  config.tool_power_draw          = param_node->declare_parameter("tool_power_draw",          0.0);
  config.recharge_threshold       = param_node->declare_parameter("recharge_threshold",       0.2);
  config.recharge_soc             = param_node->declare_parameter("recharge_soc",             1.0);

  if (config.nav_graph_path.empty()) 
  {
    RCLCPP_FATAL(param_node->get_logger(),"nav_graph_path parameter is required"); rclcpp::shutdown();
    return 1;
  }

  RCLCPP_INFO(param_node->get_logger(),
    "Starting VDA5050 Fleet Adapter: fleet=%s, mqtt=%s",
    config.fleet_name.c_str(), config.mqtt_broker_url.c_str());

  vda5050_fleet_adapter::FleetAdapter adapter(config);
  adapter.run();

  rclcpp::shutdown();
  return 0;
}
