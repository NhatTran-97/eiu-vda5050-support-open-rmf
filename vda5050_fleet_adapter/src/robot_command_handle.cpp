#include <vda5050_fleet_adapter/robot_command_handle.hpp>

#include <rclcpp/rclcpp.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <random>

namespace vda5050_fleet_adapter {

RobotCommandHandle::RobotCommandHandle(
  const std::string& robot_name,
  const std::string& manufacturer,
  const std::string& serial_number,
  const std::string& mqtt_broker_url,
  const std::string& interface_name,
  const std::string& map_name)
: _robot_name(robot_name),
  _manufacturer(manufacturer),
  _serial_number(serial_number),
  _map_name(map_name)
{
  const std::string prefix =
    interface_name + "/v2/" + manufacturer + "/" + serial_number;

  _order_topic          = prefix + "/order";
  _instant_actions_topic = prefix + "/instantActions";
  _state_topic          = prefix + "/state";

  _mqtt = std::make_shared<mqtt::async_client>(
    mqtt_broker_url, robot_name + "_fleet_adapter");
  _mqtt->set_callback(*this);

  mqtt::connect_options opts;
  opts.set_clean_session(true);
  opts.set_keep_alive_interval(60);

  try {
    _mqtt->connect(opts)->wait();
    _mqtt->subscribe(_state_topic, 0)->wait();
    RCLCPP_INFO(rclcpp::get_logger(_robot_name),
      "MQTT connected, subscribed to %s", _state_topic.c_str());
  } catch (const mqtt::exception& e) {
    RCLCPP_ERROR(rclcpp::get_logger(_robot_name),
      "MQTT connect failed: %s", e.what());
  }
}

RobotCommandHandle::~RobotCommandHandle()
{
  if (_mqtt && _mqtt->is_connected())
    _mqtt->disconnect()->wait();
}

void RobotCommandHandle::set_updater(std::shared_ptr<RobotUpdateHandle> updater)
{
  std::lock_guard<std::mutex> lock(_mutex);
  _updater = updater;
}

// ─── RobotCommandHandle interface ────────────────────────────────────────────

void RobotCommandHandle::follow_new_path(
  const std::vector<rmf_traffic::agv::Plan::Waypoint>& waypoints,
  ArrivalEstimator next_arrival_estimator,
  RequestCompleted path_finished_callback)
{
  std::lock_guard<std::mutex> lock(_mutex);

  _path_active       = true;
  _arrival_estimator = std::move(next_arrival_estimator);
  _path_finished_cb  = std::move(path_finished_callback);
  _current_order_id  = _make_order_id();
  _expected_node_count = waypoints.size();

  RCLCPP_INFO(rclcpp::get_logger(_robot_name),
    "follow_new_path: %zu waypoints, order_id=%s",
    waypoints.size(), _current_order_id.c_str());

  const auto order = _build_order(waypoints, _current_order_id);
  _publish(_order_topic, order);
}

void RobotCommandHandle::stop()
{
  // No-op: RMF calls stop() then follow_new_path immediately.
  // Sending cancelOrder causes a timing race where the new order arrives before
  // cancelOrder is FINISHED, blocking it. Instead we use VDA5050 order-update
  // (same orderId, higher orderUpdateId) in follow_new_path to replace the path.
  RCLCPP_INFO(rclcpp::get_logger(_robot_name), "stop() called");
}

void RobotCommandHandle::dock(
  const std::string& dock_name,
  RequestCompleted docking_finished_callback)
{
  RCLCPP_INFO(rclcpp::get_logger(_robot_name), "dock(%s) called", dock_name.c_str());
  // TODO: map dock_name to a VDA5050 custom action
  docking_finished_callback();
}

// ─── MQTT callbacks ───────────────────────────────────────────────────────────

void RobotCommandHandle::connected(const std::string&)
{
  RCLCPP_INFO(rclcpp::get_logger(_robot_name), "MQTT reconnected");
  _mqtt->subscribe(_state_topic, 0);
}

void RobotCommandHandle::connection_lost(const std::string& cause)
{
  RCLCPP_WARN(rclcpp::get_logger(_robot_name),
    "MQTT connection lost: %s", cause.c_str());
}

void RobotCommandHandle::message_arrived(mqtt::const_message_ptr msg)
{
  try 
  {
    const auto state = nlohmann::json::parse(msg->get_payload_str());
    _on_state(state);
  } catch (const std::exception& e) 
  {
    RCLCPP_ERROR(rclcpp::get_logger(_robot_name),
      "Failed to parse state JSON: %s", e.what());
  }
}

// ─── Private ─────────────────────────────────────────────────────────────────

void RobotCommandHandle::_on_state(const nlohmann::json& state)
{
  std::lock_guard<std::mutex> lock(_mutex);

  if (!_updater)
    return;

  // Update position from agvPosition using map-based update
  if (state.contains("agvPosition")) {
    const auto& pos = state["agvPosition"];
    const double x     = pos.value("x", 0.0);
    const double y     = pos.value("y", 0.0);
    const double theta = pos.value("theta", 0.0);
    const std::string map_id = pos.value("mapId", "map");
    _updater->update_position(map_id, {x, y, theta});
  }

  // Order-progress logic must apply ONLY to the order we currently own. A stale
  // state from a previous order (nodeStates empty, driving false) could otherwise
  // complete the freshly issued path. Position is always updated above.
  const std::string state_order_id = state.value("orderId", std::string{});
  if (!_path_active || _current_order_id.empty() ||
      state_order_id != _current_order_id) {
    return;
  }

  // Track node completion. nodeId is "n<index>" where <index> is the position in
  // the path we issued — map it back directly instead of assuming a sequenceId
  // scheme, so action-only nodes or future re-sequencing can't shift the index.
  if (state.contains("lastNodeId")) {
    const std::string last_node_id = state["lastNodeId"].get<std::string>();

    std::optional<std::size_t> reached_idx;
    if (last_node_id.size() > 1 && last_node_id[0] == 'n') {
      try {
        reached_idx = static_cast<std::size_t>(std::stoul(last_node_id.substr(1)));
      } catch (const std::exception&) {
        reached_idx = std::nullopt;
      }
    }

    if (reached_idx.has_value() && reached_idx.value() != _last_reached_idx) {
      _last_reached_idx = reached_idx.value();
      RCLCPP_INFO(rclcpp::get_logger(_robot_name),
        "Node reached: %s (waypoint index=%zu)",
        last_node_id.c_str(), reached_idx.value());
      if (_arrival_estimator)
        _arrival_estimator(reached_idx.value(), rmf_traffic::Duration(0));
    }
  }

  // Path finished when all nodes completed and robot stopped
  const bool driving = state.value("driving", true);
  const bool all_nodes_done =
    state.contains("nodeStates") && state["nodeStates"].empty();

  if (all_nodes_done && !driving) {
    RCLCPP_INFO(rclcpp::get_logger(_robot_name), "Path finished");
    _path_active = false;
    _last_reached_idx = std::numeric_limits<std::size_t>::max();
    if (_path_finished_cb) {
      _path_finished_cb();
      _path_finished_cb = nullptr;
    }
  }
}

nlohmann::json RobotCommandHandle::_build_order(
  const std::vector<rmf_traffic::agv::Plan::Waypoint>& waypoints,
  const std::string& order_id)
{
  nlohmann::json nodes = nlohmann::json::array();
  nlohmann::json edges = nlohmann::json::array();

  for (std::size_t i = 0; i < waypoints.size(); ++i) {
    const auto& wp = waypoints[i];
    const auto pos = wp.position();
    const auto idx = wp.graph_index();
    RCLCPP_INFO(rclcpp::get_logger(_robot_name),
      "  waypoint[%zu]: pos=(%.3f, %.3f) graph_idx=%s",
      i, pos[0], pos[1],
      idx.has_value() ? std::to_string(idx.value()).c_str() : "none");

    nlohmann::json node;
    node["nodeId"]     = "n" + std::to_string(i);
    node["sequenceId"] = static_cast<int>(i * 2);
    node["released"]   = true;
    node["actions"]    = nlohmann::json::array();
    node["nodePosition"] = {
      {"x",     pos[0]},
      {"y",     pos[1]},
      {"theta", pos[2]},
      {"mapId", _map_name},
      {"allowedDeviationXY",    0.5},
      {"allowedDeviationTheta", 0.3}
    };
    nodes.push_back(node);

    if (i > 0) {
      nlohmann::json edge;
      edge["edgeId"]      = "e" + std::to_string(i - 1);
      edge["sequenceId"]  = static_cast<int>(i * 2 - 1);
      edge["released"]    = true;
      edge["startNodeId"] = "n" + std::to_string(i - 1);
      edge["endNodeId"]   = "n" + std::to_string(i);
      edge["actions"]     = nlohmann::json::array();
      edges.push_back(edge);
    }
  }

  nlohmann::json order;
  order["headerId"]       = ++_header_id;
  order["timestamp"]      = _timestamp();
  order["version"]        = "2.1.0";
  order["manufacturer"]   = _manufacturer;
  order["serialNumber"]   = _serial_number;
  order["orderId"]        = order_id;
  order["orderUpdateId"]  = ++_order_update_id;
  order["nodes"]          = nodes;
  order["edges"]          = edges;

  return order;
}

void RobotCommandHandle::_publish(
  const std::string& topic,
  const nlohmann::json& payload)
{
  if (!_mqtt->is_connected()) {
    RCLCPP_WARN(rclcpp::get_logger(_robot_name), "MQTT not connected, skipping publish");
    return;
  }
  auto msg = mqtt::make_message(topic, payload.dump());
  msg->set_qos(0);
  _mqtt->publish(msg);
}

std::string RobotCommandHandle::_make_order_id()
{
  static std::atomic<int> counter{0};
  return _robot_name + "-order-" + std::to_string(++counter);
}

std::string RobotCommandHandle::_timestamp() const
{
  const auto now = std::chrono::system_clock::now();
  const auto t   = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

} // namespace vda5050_fleet_adapter
