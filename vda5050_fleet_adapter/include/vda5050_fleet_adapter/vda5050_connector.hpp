#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <mqtt/async_client.h>
#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter/transform.hpp"
#include "vda5050_fleet_adapter/vda5050_protocol.hpp"

namespace vda5050_fleet_adapter {

/// Per-robot position/battery snapshot in RMF coordinates (uplink to RMF).
struct RobotData
{
  std::string map_name;
  std::array<double, 3> position;  // x, y, theta (RMF frame)
  double battery_soc;
};

/// Owns ONE MQTT connection to the broker and tracks the latest VDA5050 'state'
/// for every robot. The fleet adapter calls:
///   navigate()            -> publishes an 'order'           (RMF -> AGV)
///   stop()                -> publishes 'instantActions'      (cancelOrder)
///   execute_instant_action-> publishes 'instantActions'      (custom action)
///   get_data()            -> reads the cached 'state'        (AGV -> RMF)
///   is_command_completed()-> checks the cached state vs the last order
///
/// Thread-safe: all per-robot data is guarded by an internal mutex. Ported from
/// the reference RobotClientAPI.py.
class Vda5050Connector : public virtual mqtt::callback
{
public:
  Vda5050Connector(rclcpp::Logger logger, std::string broker_url,
                   std::string interface_name,
                   std::optional<std::string> username = std::nullopt,
                   std::optional<std::string> password = std::nullopt);
  ~Vda5050Connector() override;

  void start();
  void shutdown();

  /// Register a robot and subscribe to its uplink topics.
  void add_robot(const std::string& name, const std::string& manufacturer,
                 const std::string& serial, const Transform& transform);

  // ── RMF -> AGV (downlink) ───────────────────────────────────────────────────
  /// Publish a single-destination order (base node = current pose, end node =
  /// destination). Coordinates are RMF; transformed to the robot frame here.
  void navigate(const std::string& name, const std::string& dest_node_id,
                double x, double y, double theta, const std::string& map_id,
                std::optional<double> speed_limit = std::nullopt);

  /// Publish a cancelOrder instantAction.
  void stop(const std::string& name);

  /// Publish a custom instantAction; returns the generated actionId.
  std::string execute_instant_action(
    const std::string& name, const std::string& action_type,
    const std::vector<std::pair<std::string, std::string>>& parameters = {});

  /// Publish a `stateRequest` instantAction to prompt the robot to publish its
  /// state immediately, so the fleet adapter can add_robot() promptly instead of
  /// waiting for the next periodic state (avoids "no robots" on early dispatch).
  void request_state(const std::string& name);

  // ── AGV -> RMF (uplink) ─────────────────────────────────────────────────────
  std::optional<RobotData> get_data(const std::string& name);
  bool is_command_completed(const std::string& name);
  std::optional<std::string> get_action_state(const std::string& name,
                                               const std::string& action_id);
  bool is_online(const std::string& name, double state_timeout_s = 10.0);

  // ── mqtt::callback ──────────────────────────────────────────────────────────
  void connected(const std::string& cause) override;
  void connection_lost(const std::string& cause) override;
  void message_arrived(mqtt::const_message_ptr msg) override;

private:
  struct RobotContext
  {
    std::string name;
    std::string manufacturer;
    std::string serial;
    std::string interface_name;
    Transform transform;
    int header_id = 0;
    std::string current_order_id;
    std::string target_node_id;
    std::optional<protocol::ParsedState> last_state;
    std::string last_node_id;
    std::optional<bool> connected;  // nullopt = unknown
    std::chrono::steady_clock::time_point last_state_time{};

    int next_header() { return header_id++; }
  };

  void subscribe_robot(const RobotContext& ctx);
  void publish(const RobotContext& ctx, const std::string& leaf,
               const nlohmann::json& message);
  RobotContext* match_robot(const std::string& topic);  // call under _mutex

  rclcpp::Logger _logger;
  std::string _interface_name;
  std::shared_ptr<mqtt::async_client> _client;
  mqtt::connect_options _conn_opts;

  std::mutex _mutex;
  std::map<std::string, std::unique_ptr<RobotContext>> _robots;
  std::atomic<bool> _shutdown{false};
};

}  // namespace vda5050_fleet_adapter
