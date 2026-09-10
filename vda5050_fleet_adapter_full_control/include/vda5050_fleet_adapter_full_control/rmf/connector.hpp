#ifndef CONNECTOR_HPP
#define CONNECTOR_HPP

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <array>
#include <vector>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

// Per-robot position/battery snapshot in RMF coordinates (uplink to RMF).
struct RobotData
{
    std::string map_name;
    std::array<double, 3> position;  // x, y, theta (RMF frame)
    double battery_soc;
};

// Owns one MqttClient and tracks the latest VDA5050 'state' for every
// robot. The fleet adapter calls:
//   navigate()             -> publishes an 'order'            (RMF -> AGV)
//   stop()                 -> publishes 'instantActions'      (cancelOrder)
//   execute_instant_action -> publishes 'instantActions'      (custom action)
//   get_data()              -> reads the cached 'state'        (AGV -> RMF)
//   is_command_completed() -> checks the cached state vs the last order
class Connector
{
public:
    Connector(rclcpp::Logger logger, std::string broker_url,
              std::string interface_name,
              std::optional<std::string> username = std::nullopt,
              std::optional<std::string> password = std::nullopt);
    ~Connector();

    void start();
    void shutdown();

    // Register a robot and subscribe to its uplink topics.
    void add_robot(const std::string &name, const std::string &manufacturer,
                   const std::string &serial, const Transform &transform);

    // ── RMF -> AGV (downlink) ───────────────────────────────────────────
    // Publish a single-destination order (base node = current pose, end
    // node = destination). Coordinates are RMF; transformed to the robot
    // frame here.
    void navigate(const std::string &name, const std::string &dest_node_id,
                  double x, double y, double theta, const std::string &map_id,
                  std::optional<double> speed_limit = std::nullopt);

    // Publish a cancelOrder instantAction: the AGV drops the current order
    // entirely. Use pause() instead for a hold the robot can resume from.
    void stop(const std::string &name);

    // Publish a startPause instantAction: the AGV stops moving but keeps its
    // current order, so resume() can continue it. Unlike stop(), the tracked
    // order id is deliberately kept, so is_command_completed() still refers
    // to the order the robot is holding.
    void pause(const std::string &name);

    // Publish a stopPause instantAction, continuing the order the AGV was
    // holding after pause().
    void resume(const std::string &name);

    // Publish a custom instantAction; returns the generated actionId.
    std::string execute_instant_action(
        const std::string &name, const std::string &action_type,
        const std::vector<std::pair<std::string, std::string>> &parameters = {});

    // Publish a stateRequest instantAction so the robot reports its state
    // immediately, instead of waiting for the next periodic state.
    void request_state(const std::string &name);

    // Publish an initPosition instantAction telling the AGV to re-seed its
    // localization at (x, y, theta), given in RMF coordinates and converted
    // to the robot frame here. Returns the generated actionId so the caller
    // can follow it with get_action_state(), or an empty string if the robot
    // is unknown. AGVs commonly refuse this while an order is running.
    std::string init_position(const std::string &name, double x, double y,
                              double theta, const std::string &map_id);

    // ── AGV -> RMF (uplink) ──────────────────────────────────────────────
    std::optional<RobotData> get_data(const std::string &name);
    bool is_command_completed(const std::string &name);
    std::optional<std::string> get_action_state(const std::string &name,
                                                const std::string &action_id);
    bool is_online(const std::string &name, double state_timeout_s = 10.0);

private:
    struct RobotContext
    {
        std::string name;
        std::string manufacturer;
        std::string serial;
        std::string interface_name;
        Transform transform;
        // VDA5050 defines headerId per topic (monotonically +1 per message
        // sent on that topic); order and instantActions are separate
        // topics, so each needs its own counter.
        int order_header_id = 0;
        int instant_actions_header_id = 0;
        std::string current_order_id;
        std::string target_node_id;
        std::optional<vda5050::ParsedState> last_state;
        // What the AGV declared about itself. Arrives once per MQTT session:
        // the robot publishes it retained on connect, so subscribing is
        // enough -- this adapter never has to ask for it.
        std::optional<vda5050::ParsedFactsheet> factsheet;
        std::string last_node_id;
        std::optional<bool> connected;  // nullopt = unknown
        std::chrono::steady_clock::time_point last_state_time{};

        // Log throttles: remember what was last reported so a 10 Hz state
        // stream produces one line per change instead of one line per tick.
        std::string last_incomplete_key;
        std::string last_errors_key;

        int next_order_header() { return order_header_id++; }
        int next_instant_actions_header() { return instant_actions_header_id++; }
    };

    // Subscribe to a robot's uplink topics.
    void subscribe_robot(const RobotContext &ctx);
    // Find the robot whose manufacturer/serial appear in `topic`. Call
    // under _mutex.
    RobotContext *match_robot(const std::string &topic);
    // Publish a fully-formed payload to a topic, logging (not throwing) if
    // it gets dropped.
    void publish_raw(const std::string &topic, const std::string &payload);
    // The blockingType to send this robot for `action_type`: whatever its
    // factsheet declares, or `preferred` when no factsheet has arrived yet.
    // Call under _mutex.
    static std::string blocking_type_for(const RobotContext &ctx,
                                         const std::string &action_type,
                                         const std::string &preferred);
    // Log anything about a pending navigation that looks unusable (empty
    // ids, non-finite pose, a speed limit the AGV's factsheet says it cannot
    // do). Warn-only by design -- see the call site. Call under _mutex.
    void warn_if_unroutable(const RobotContext &ctx, const std::string &dest_node_id,
                            double x, double y, double theta,
                            const std::string &map_id,
                            std::optional<double> speed_limit) const;

    // Wired to _mqtt_client's callbacks.
    void on_connected();
    void on_connection_lost(const std::string &cause);
    void on_error(const std::string &context, const std::string &what);
    void handle_message(const std::string &topic, const std::string &payload);

    rclcpp::Logger _logger;
    std::string _interface_name;
    mqtt::MqttClient _mqtt_client;

    std::mutex _mutex;
    std::map<std::string, std::unique_ptr<RobotContext>> _robots;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // CONNECTOR_HPP
