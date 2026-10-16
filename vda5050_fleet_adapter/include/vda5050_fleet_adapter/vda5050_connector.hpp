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

#include "vda5050_fleet_adapter/factsheet.hpp"
#include "vda5050_fleet_adapter/readiness.hpp"
#include "vda5050_fleet_adapter/transform.hpp"
#include "vda5050_fleet_adapter/vda5050_protocol.hpp"

namespace vda5050_fleet_adapter {

/// Outcome of navigate(): the order was queued, or why it was not sent.
enum class NavigateStatus { queued, rejected, transport_failed };

/// Per-robot position/battery snapshot in RMF coordinates (uplink to RMF).
struct RobotData
{
  std::string map_name;
  std::array<double, 3> position;  // x, y, theta (RMF frame)
  double battery_soc;
};

/// One MQTT connection for all robots of the fleet; keeps the latest VDA5050 state of each robot.
/// Downlink: order and instantActions. Uplink: state, connection, factsheet.

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
  NavigateStatus navigate(const std::string& name, const std::string& dest_node_id,
                double x, double y, double theta, const std::string& map_id,
                std::optional<double> speed_limit = std::nullopt);

  /// Publish a cancelOrder instantAction.
  void stop(const std::string& name);

  /// Publish a custom instantAction; returns the generated actionId.
  std::string execute_instant_action(
    const std::string& name, const std::string& action_type,
    const std::vector<std::pair<std::string, std::string>>& parameters = {});

  /// Publish a `stateRequest` instantAction so the AGV reports its state at once.
  void request_state(const std::string& name);

  /// Reject custom actions and orders that fail a hard check (default); off only warns.
  void set_strict_validation(bool strict);

  /// Send a new destination as an update of the live order (same orderId, next orderUpdateId).
  void set_stitch_on_replan(bool enabled);

  /// Publish startPause; false when nothing was queued.
  bool pause(const std::string& name);

  /// Publish stopPause; false when nothing was queued.
  bool resume(const std::string& name);

  /// Publish initPosition (RMF frame); returns its actionId, or empty on failure.
  std::string init_position(const std::string& name, double x, double y,
                            double theta, const std::string& map_id);

  /// Cap the speed of new orders for `name`; nullopt removes the cap.
  /// Returns false for an unknown robot.
  bool set_speed_limit(const std::string& name, std::optional<double> limit);

  /// Ask for a missing factsheet, with retries; call periodically.
  void poll(const std::string& name);

  // ── AGV -> RMF (uplink) ─────────────────────────────────────────────────────
  std::optional<RobotData> get_data(const std::string& name);
  bool is_command_completed(const std::string& name);
  std::optional<std::string> get_action_state(const std::string& name,
                                               const std::string& action_id);

  /// An action's status and result description, once the AGV reports it.
  std::optional<std::pair<std::string, std::string>> get_action_result(
    const std::string& name, const std::string& action_id);

  /// The latest map the AGV reported, even when it has no usable pose.
  std::optional<std::string> get_known_map(const std::string& name);
  bool is_online(const std::string& name, double state_timeout_s = 10.0);

  /// Whether `name` can take tasks from RMF, and why not.
  Readiness readiness(const std::string& name, bool tolerate_pause = false);

  /// The orderId last published for `name`; empty when none is tracked.
  std::string current_order_id(const std::string& name);

  /// The orderUpdateId last published for `name`'s current order; 0 for a new order.
  int current_order_update_id(const std::string& name);

  /// True while `name` still runs an unfinished order to `dest_node_id`.
  bool has_active_order_to(const std::string& name, const std::string& dest_node_id);

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
    // headerId counts per topic (VDA5050), so order and instantActions have separate counters.
    int order_header_id = 0;
    int instant_actions_header_id = 0;
    std::string current_order_id;
    int order_update_id = 0;
    // Base node of the currently live order, kept fixed across stitched updates.
    std::string base_node_id;
    std::array<double, 3> base_pose{};
    std::chrono::steady_clock::time_point last_order_time{};
    std::string target_node_id;
    std::optional<protocol::ParsedState> last_state;
    std::string last_node_id;
    std::optional<bool> connected;  // nullopt = unknown
    std::chrono::steady_clock::time_point last_state_time{};

    std::optional<protocol::ParsedFactsheet> factsheet;
    int factsheet_requests = 0;
    std::chrono::steady_clock::time_point factsheet_wait_since{};
    std::optional<double> speed_limit;

    // Last logged values; a change is logged once.
    std::string last_incomplete_key;
    std::string last_errors_key;
    std::string last_safety_key;
    std::string last_mode;

    int next_order_header() { return order_header_id++; }
    int next_instant_actions_header() { return instant_actions_header_id++; }
  };

  /// Subscribe to a robot's uplink topics. Call without holding _mutex (Paho calls message_arrived).
  void subscribe_robot(const RobotContext& ctx);

protected:
  /// Publish a payload to a topic; false when it was not queued. Call without holding _mutex.
  virtual bool publish_raw(const std::string& topic, const std::string& payload);

private:

  /// Blocking type for an action: the factsheet's choice, else `preferred`.
  static std::string blocking_type_for(const RobotContext& ctx, const std::string& action_type, const std::string& preferred);

  /// Publish one instant action; false when it was not queued.
  bool send_instant_action(const std::string& name, const std::string& action_type, const std::string& preferred_blocking, const char* label);
  RobotContext* match_robot(const std::string& topic);  // call under _mutex

  rclcpp::Logger _logger;
  std::string _interface_name;
  std::shared_ptr<mqtt::async_client> _client;
  mqtt::connect_options _conn_opts;

  std::mutex _mutex;
  std::map<std::string, std::unique_ptr<RobotContext>> _robots;
  std::atomic<bool> _shutdown{false};
  bool _strict_validation = true;
  bool _stitch_on_replan = false;
};

}  // namespace vda5050_fleet_adapter
