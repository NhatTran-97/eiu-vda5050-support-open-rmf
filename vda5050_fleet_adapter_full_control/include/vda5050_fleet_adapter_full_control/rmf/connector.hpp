#ifndef CONNECTOR_HPP
#define CONNECTOR_HPP

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <array>
#include <utility>
#include <vector>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

// Whether MQTT accepted a downlink message for queuing; this does not confirm delivery.
enum class CommandStatus
{
    queued,
    transport_failed,
};

// Per-robot state in RMF coordinates, assembled from VDA5050 state and visualization messages.
struct RobotData
{
    std::string map_name;
    std::array<double, 3> position;  // x, y, theta (RMF frame)
    double battery_soc;
    bool charging = false;
    // Order currently reported by the AGV; empty while idle.
    std::string order_id;
    // Last node reported by the AGV.
    std::string last_node_id;
    // Sequence identifier of last_node_id within its order.
    std::optional<std::uint32_t> last_node_sequence_id;

    // Measured velocity in the AGV frame, when reported.
    std::optional<vda5050::Velocity> velocity;
    // Distance travelled since last_node_id, in metres.
    std::optional<double> distance_since_last_node;

    // AUTOMATIC | SEMIAUTOMATIC | MANUAL | SERVICE | TEACHIN.
    std::string operating_mode = "AUTOMATIC";
    // True when operating_mode is one a master may send orders in.
    bool operable = true;
    vda5050::SafetyState safety_state;
    // First FATAL error type reported by the AGV.
    std::string fatal_error;
    // Whether the AGV reports a pause, regardless of who requested it.
    bool paused = false;
    // The AGV is asking for more of the order horizon to be released.
    bool new_base_request = false;
    // agvPosition.localizationScore, when the AGV scores its localization.
    std::optional<double> localization_score;

    // True when the AGV can accept master-control orders.
    bool ready_for_orders() const
    {
        return operable && !safety_state.triggered() && fatal_error.empty() && !paused;
    }
};

// Connect RMF commands with VDA5050 MQTT messages and track each robot's protocol state.
class Connector
{
public:
    Connector(rclcpp::Logger logger, std::string broker_url, std::string interface_name,
              std::optional<std::string> username = std::nullopt,
              std::optional<std::string> password = std::nullopt);
    ~Connector();

    void start();
    void shutdown();

    // Registers a robot and subscribes to its uplink topics.
    void add_robot(const std::string &name, const std::string &manufacturer,
                   const std::string &serial, const Transform &transform);

    // Route waypoint in RMF coordinates.
    struct RoutePoint
    {
        std::string node_id;
        double x = 0.0;
        double y = 0.0;
        double theta = 0.0;
        std::optional<double> speed_limit;
    };

    // Publish a multi-node order and track completion at its final waypoint.
    struct NavigateResult
    {
        CommandStatus status = CommandStatus::queued;
        std::string order_id;
    };

    // Release this many route points; nullopt releases the full route and the remainder stays in the horizon.
    NavigateResult navigate_route(const std::string &name,
                                  const std::vector<RoutePoint> &route,
                                  const std::string &map_id,
                                  std::optional<std::size_t> released_count = std::nullopt);

    // Extend an active order with a larger released horizon and a new orderUpdateId.
    CommandStatus release_more(const std::string &name, std::size_t released_count);

    // Set or clear the operator speed cap applied to subsequent route edges.
    bool set_speed_limit(const std::string &name, std::optional<double> limit);

    // Returns the configured operator speed cap, when available.
    std::optional<double> speed_limit(const std::string &name) const;

    // Publishes cancelOrder and clears local tracking after MQTT accepts it.
    CommandStatus stop(const std::string &name);

    // Publishes startPause while retaining the tracked order.
    CommandStatus pause(const std::string &name);

    // Publishes stopPause to continue a paused order.
    CommandStatus resume(const std::string &name);

    // Publish an instant action and return its ID, or an empty string if dispatch fails.
    std::string execute_instant_action(
        const std::string &name, const std::string &action_type,
        const nlohmann::json &parameters = nlohmann::json::object());

    // Requests an immediate state update.
    void request_state(const std::string &name);

    // Send initPosition in the robot frame and return its action ID, or an empty string on failure.
    std::string init_position(const std::string &name, double x, double y,
                              double theta, const std::string &map_id);

    // State received from the AGV.
    std::optional<RobotData> get_data(const std::string &name);
    bool is_command_completed(const std::string &name);

    // Detect an order the AGV has not acknowledged within the timeout.
    bool is_order_stuck(const std::string &name, double timeout_s = 15.0) const;
    std::optional<std::string> get_action_state(const std::string &name,
                                                const std::string &action_id);
    // Status and resultDescription of `action_id` as last reported by the AGV.
    std::optional<std::pair<std::string, std::string>> get_action_result(
        const std::string &name, const std::string &action_id);
    // Best-known map for `name`, without requiring the AGV to be localized.
    std::optional<std::string> get_known_map(const std::string &name);
    bool is_online(const std::string &name, double state_timeout_s = 10.0);

private:
    struct RobotContext
    {
        std::string name;
        std::string manufacturer;
        std::string serial;
        std::string interface_name;
        // MQTT topic suffix used to identify this robot.
        std::string mqtt_needle;
        Transform transform;
        // VDA5050 header counters are maintained independently per topic.
        int order_header_id = 0;
        int instant_actions_header_id = 0;
        // Timestamp used to check the factsheet's minimum order interval.
        std::chrono::steady_clock::time_point last_order_time{};
        // Operator speed cap for subsequent orders.
        std::optional<double> operator_speed_limit;
        std::string current_order_id;
        std::string target_node_id;
        // Actions associated with the tracked order.
        std::vector<std::string> order_action_ids;
        // Update ID of the tracked order, incremented for each horizon extension.
        int order_update_id = 0;
        // Robot-frame route data retained to build later horizon updates.
        std::vector<vda5050::RouteWaypoint> current_route;
        std::string current_base_id;
        vda5050::RobotPose current_base;
        std::string current_map_id;
        // Number of route points released in the last published order.
        std::size_t current_released_count = 0;
        std::optional<vda5050::ParsedState> last_state;
        // Visualization data is used only to refine pose and velocity.
        std::optional<vda5050::ParsedVisualization> last_visualization;
        std::chrono::steady_clock::time_point last_visualization_time{};
        // Capabilities declared by the AGV.
        std::optional<vda5050::ParsedFactsheet> factsheet;
        std::string last_node_id;
        std::optional<bool> connected;  // nullopt = unknown
        std::chrono::steady_clock::time_point last_state_time{};

        // Last reported values used to suppress duplicate log messages.
        std::string last_incomplete_key;
        std::string last_errors_key;
        std::string last_safety_key;
        std::string last_mode_key;
        std::string last_info_key;
        std::string last_loads_key;
        std::string last_maps_key;
        bool last_new_base_request = false;

        int next_order_header() { return order_header_id++; }
        int next_instant_actions_header() { return instant_actions_header_id++; }
    };

    // Subscribes to a robot's uplink topics.
    void subscribe_robot(const RobotContext &ctx);

    // Find a robot by MQTT topic; the caller holds _mutex.
    RobotContext *match_robot(const std::string &topic);

    // Publishes a serialized payload without throwing.
    CommandStatus publish_raw(const std::string &topic, const std::string &payload);

    // Choose a factsheet-compatible blocking type; the caller holds _mutex.
    static std::string blocking_type_for(const RobotContext &ctx,
                                         const std::string &action_type,
                                         const std::string &preferred);
                                         
    // Report unsupported navigation inputs; the caller holds _mutex.
    void warn_if_unroutable(const RobotContext &ctx, const std::string &dest_node_id,
                            double x, double y, double theta,
                            const std::string &map_id,
                            std::optional<double> speed_limit) const;

    // Report action conflicts; the caller holds _mutex.
    void warn_if_action_conflicts(const RobotContext &ctx, const std::string &action_type,
                                  const std::string &blocking_type) const;

    // Check factsheet protocol limits and update the order timestamp; the caller holds _mutex.
    void warn_if_order_oversized(RobotContext &ctx, std::size_t node_count,
                                 std::size_t edge_count) const;

    // Report a map ID mismatch with the AGV; the caller holds _mutex.
    void warn_if_map_mismatch(const RobotContext &ctx, const std::string &order_map_id) const;

    // Report changes in cached operational state; the caller holds _mutex.
    void report_state_changes(RobotContext &ctx);

    // Handle callbacks from the MQTT client.
    void on_connected();
    void on_connection_lost(const std::string &cause);
    void on_error(const std::string &context, const std::string &what);
    void handle_message(const std::string &topic, const std::string &payload);

    rclcpp::Logger _logger;
    std::string _interface_name;
    mqtt::MqttClient _mqtt_client;

    // Protects robot state shared by MQTT, ROS, and update-loop threads.
    mutable std::mutex _mutex;
    std::map<std::string, std::unique_ptr<RobotContext>> _robots;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // CONNECTOR_HPP
