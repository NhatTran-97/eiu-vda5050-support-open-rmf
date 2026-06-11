#pragma once

#include <rmf_fleet_adapter/agv/RobotCommandHandle.hpp>
#include <rmf_fleet_adapter/agv/RobotUpdateHandle.hpp>
#include <rmf_traffic/agv/Planner.hpp>

#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>

#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <optional>
#include <thread>
#include <limits>

namespace vda5050_fleet_adapter {

class RobotCommandHandle: public rmf_fleet_adapter::agv::RobotCommandHandle,
    public mqtt::callback
{
public:
  using SharedPtr = std::shared_ptr<RobotCommandHandle>;
  using RobotUpdateHandle = rmf_fleet_adapter::agv::RobotUpdateHandle;
  using ArrivalEstimator =
    rmf_fleet_adapter::agv::RobotCommandHandle::ArrivalEstimator;
  using RequestCompleted =
    rmf_fleet_adapter::agv::RobotCommandHandle::RequestCompleted;

  RobotCommandHandle(
    const std::string& robot_name,
    const std::string& manufacturer,
    const std::string& serial_number,
    const std::string& mqtt_broker_url,
    const std::string& interface_name = "uagv",
    const std::string& map_name = "tb3_world");

  ~RobotCommandHandle() override;

  void set_updater(std::shared_ptr<RobotUpdateHandle> updater);

  // RobotCommandHandle interface
  void follow_new_path(
    const std::vector<rmf_traffic::agv::Plan::Waypoint>& waypoints,
    ArrivalEstimator next_arrival_estimator,
    RequestCompleted path_finished_callback) override;

  void stop() override;

  void dock(
    const std::string& dock_name,
    RequestCompleted docking_finished_callback) override;

  // mqtt::callback interface
  void message_arrived(mqtt::const_message_ptr msg) override;
  void connection_lost(const std::string& cause) override;
  void connected(const std::string& cause) override;

private:
  std::string _robot_name;
  std::string _manufacturer;
  std::string _serial_number;
  std::string _map_name;
  std::string _order_topic;
  std::string _instant_actions_topic;
  std::string _state_topic;

  std::shared_ptr<mqtt::async_client> _mqtt;
  std::shared_ptr<RobotUpdateHandle> _updater;

  std::mutex _mutex;
  std::string _current_order_id;
  std::size_t _expected_node_count{0};
  RequestCompleted _path_finished_cb;
  ArrivalEstimator _arrival_estimator;
  std::atomic<bool> _path_active{false};
  std::size_t _last_reached_idx{std::numeric_limits<std::size_t>::max()};

  int _header_id{0};
  int _order_update_id{0};

  void _on_state(const nlohmann::json& state);

  nlohmann::json _build_order(
    const std::vector<rmf_traffic::agv::Plan::Waypoint>& waypoints,
    const std::string& order_id);

  void _publish(const std::string& topic, const nlohmann::json& payload);

  std::string _make_order_id();
  std::string _timestamp() const;
};

} // namespace vda5050_fleet_adapter
