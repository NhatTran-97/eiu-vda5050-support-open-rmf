/// RMF <-> VDA5050 fleet adapter (EasyFullControl, C++).
///
/// RMF Core is unchanged. This node:
///   * builds an EasyFullControl fleet from config.yaml + nav_graph.yaml,
///   * for each robot wires RMF callbacks to the VDA5050/MQTT connector,
///   * runs an update loop that pushes each robot's VDA5050 state into RMF.
///
/// Usage:
///   ros2 run vda5050_fleet_adapter fleet_adapter -c <config.yaml> -n <nav_graph.yaml>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <Eigen/Geometry>
#include <yaml-cpp/yaml.h>

#include <rclcpp/rclcpp.hpp>
#include <rmf_fleet_adapter/agv/Adapter.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>

#include "vda5050_fleet_adapter/robot_adapter.hpp"
#include "vda5050_fleet_adapter/transform.hpp"
#include "vda5050_fleet_adapter/vda5050_connector.hpp"

namespace {

using rmf_fleet_adapter::agv::Adapter;
using rmf_fleet_adapter::agv::EasyFullControl;

struct Args
{
  std::string config_file;
  std::string nav_graph;
};

Args parse_args(int argc, char** argv)
{
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "-c" || arg == "--config_file") && i + 1 < argc)
      a.config_file = argv[++i];
    else if ((arg == "-n" || arg == "--nav_graph") && i + 1 < argc)
      a.nav_graph = argv[++i];
  }
  return a;
}

}  // namespace

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  const Args args = parse_args(argc, argv);

  auto adapter = Adapter::make("vda5050_fleet_adapter");
  if (!adapter) {
    std::fprintf(stderr, "Failed to create RMF adapter (schedule node?)\n");
    return 1;
  }
  const auto logger = adapter->node()->get_logger();

  if (args.config_file.empty() || args.nav_graph.empty()) {
    RCLCPP_FATAL(logger, "Required: -c <config.yaml> -n <nav_graph.yaml>");
    return 1;
  }

  adapter->start();

  // ── EasyFullControl fleet from the rmf_fleet block of config + nav graph ──
  auto fleet_config = EasyFullControl::FleetConfiguration::from_config_files(
    args.config_file, args.nav_graph);
  if (!fleet_config) {
    RCLCPP_FATAL(logger, "Failed to parse fleet configuration from %s",
                 args.config_file.c_str());
    return 1;
  }
  auto fleet = adapter->add_easy_fleet(*fleet_config);

  // ── VDA5050 / MQTT settings (the `vda5050:` block, read by this adapter) ──
  const YAML::Node root = YAML::LoadFile(args.config_file);
  const YAML::Node vda = root["vda5050"];
  const std::string interface_name =
    vda["interface_name"] ? vda["interface_name"].as<std::string>() : "uagv";
  const double update_rate_hz =
    vda["update_rate_hz"] ? vda["update_rate_hz"].as<double>() : 10.0;

  const YAML::Node mqtt = vda["mqtt"];
  const std::string host =
    (mqtt && mqtt["host"]) ? mqtt["host"].as<std::string>() : "localhost";
  const int port = (mqtt && mqtt["port"]) ? mqtt["port"].as<int>() : 1883;
  const std::string broker_url =
    "tcp://" + host + ":" + std::to_string(port);
  std::optional<std::string> user, pass;
  if (mqtt && mqtt["username"] && !mqtt["username"].IsNull())
    user = mqtt["username"].as<std::string>();
  if (mqtt && mqtt["password"] && !mqtt["password"].IsNull())
    pass = mqtt["password"].as<std::string>();

  using vda5050_fleet_adapter::RobotAdapter;
  using vda5050_fleet_adapter::Transform;
  using vda5050_fleet_adapter::Vda5050Connector;

  auto connector = std::make_shared<Vda5050Connector>(
    logger, broker_url, interface_name, user, pass);
  connector->start();

  const YAML::Node robots_cfg = vda["robots"];
  std::map<std::string, std::shared_ptr<RobotAdapter>> robots;
  for (const auto& name : fleet_config->known_robots()) {
    const YAML::Node rc = robots_cfg ? robots_cfg[name] : YAML::Node();
    const std::string manufacturer =
      (rc && rc["manufacturer"]) ? rc["manufacturer"].as<std::string>() : "unknown";
    const std::string serial =
      (rc && rc["serial"]) ? rc["serial"].as<std::string>() : name;

    Transform tf;
    if (rc && rc["transform"]) {
      const auto t = rc["transform"];
      const double rot = t["rotation"] ? t["rotation"].as<double>() : 0.0;
      const double scale = t["scale"] ? t["scale"].as<double>() : 1.0;
      double tx = 0.0, ty = 0.0;
      if (t["translation"] && t["translation"].size() >= 2) {
        tx = t["translation"][0].as<double>();
        ty = t["translation"][1].as<double>();
      }
      tf = Transform(rot, scale, tx, ty);
    }

    connector->add_robot(name, manufacturer, serial, tf);
    robots[name] = std::make_shared<RobotAdapter>(logger, name, *connector);
  }

  // ── Update loop: VDA5050 state -> RMF ────────────────────────────────────
  std::atomic<bool> running{true};
  const auto period = std::chrono::duration<double>(1.0 / update_rate_hz);

  std::thread update_thread([&] {
    while (running && rclcpp::ok()) {
      for (auto& [name, robot] : robots) {
        try {
          const auto data = connector->get_data(name);
          if (!data)
            continue;  // no valid VDA5050 position yet
          EasyFullControl::RobotState state(
            data->map_name,
            Eigen::Vector3d(data->position[0], data->position[1],
                            data->position[2]),
            data->battery_soc);

          if (!robot->added()) {
            auto handle = fleet->add_robot(
              name, state,
              *fleet_config->get_known_robot_configuration(name),
              robot->make_callbacks());
            if (handle) {
              robot->set_update_handle(handle);
              RCLCPP_INFO(logger, "Robot '%s' added to RMF fleet", name.c_str());
            }
          } else {
            robot->update(state);
          }
        } catch (const std::exception& e) {
          RCLCPP_ERROR(logger, "update_loop error for '%s': %s", name.c_str(),
                       e.what());
        }
      }
      std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::milliseconds>(period));
    }
  });

  adapter->wait();  // blocks until rclcpp shutdown

  // Release our own resources (update loop, then MQTT link) and exit. We skip the
  // RMF Adapter and paho destructors, which can block for several seconds joining
  // internal threads, by exiting the process directly once cleanup is done.
  running = false;
  if (update_thread.joinable())
    update_thread.join();
  connector->shutdown();
  rclcpp::shutdown();
  RCLCPP_INFO(logger, "[vda5050_fleet_adapter] shutdown complete");
  std::fflush(nullptr);
  std::_Exit(0);
}
