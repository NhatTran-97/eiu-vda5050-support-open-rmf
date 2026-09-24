#ifndef CONNECTOR_HPP
#define CONNECTOR_HPP

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <array>
#include <utility>
#include <vector>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"
#include "vda5050_fleet_adapter_full_control/util/log_throttle.hpp"
#include "vda5050_fleet_adapter_full_control/util/metrics.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/cancel_tracker.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_sequence.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/link_policy.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

// Whether MQTT accepted a downlink message for queuing; this does not confirm delivery.
enum class CommandStatus
{
    queued,
    transport_failed,
    rejected,
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

    // True when the AGV can accept master-control orders; tolerate_pause ignores a pause.
    bool ready_for_orders(bool tolerate_pause = false) const
    {
        return operable && !safety_state.triggered() && fatal_error.empty() && (tolerate_pause || !paused);
    }
};

// Connect RMF commands with VDA5050 MQTT messages and track each robot's protocol state.
class Connector
{
public:
    Connector(const rclcpp::Logger &logger, const std::string &broker_url, std::string interface_name,
              std::optional<std::string> username = std::nullopt,
              std::optional<std::string> password = std::nullopt,
              const mqtt::MqttOptions &mqtt_options = {});
    ~Connector();

    void start();
    void shutdown();

    // Registers a robot and subscribes to its uplink topics.
    void add_robot(const std::string &name, const std::string &manufacturer,const std::string &serial, const Transform &transform);

    // A robot seen on this interface that is not registered.
    struct DiscoveredRobot
    {
        std::string manufacturer;
        std::string serial;
        bool connection_seen = false;
        bool online = false;
        std::optional<vda5050::ParsedFactsheet> factsheet;
        bool has_state = false;
        bool pose_initialized = false;
        double x = 0.0;
        double y = 0.0;
        double theta = 0.0;
        std::string map_id;
        // Its factsheet and state topics are subscribed.
        bool watched = false;
    };

    // Unregistered robots seen on this interface.
    std::vector<DiscoveredRobot> discovered() const;
    std::optional<DiscoveredRobot> find_discovered(const std::string &manufacturer, const std::string &serial) const;
    // Subscribe to the factsheet and state of newly seen online robots; call it outside MQTT callbacks.
    void watch_discovered();
    // Factsheets of the registered robots that published one.
    std::vector<vda5050::ParsedFactsheet> registered_factsheets() const;

    // Entry point for every message received from the broker.
    void handle_message(const std::string &topic, const std::string &payload);

    // Message path health as JSON: state ages, message and drop counts, MQTT counts, latency since the last call.
    nlohmann::json metrics();

    // Route waypoint in RMF coordinates.
    struct RoutePoint
    {
        std::string node_id;
        double x = 0.0;
        double y = 0.0;
        double theta = 0.0;
        std::optional<double> speed_limit;
    };

    // Outcome of navigate_route.
    struct NavigateResult
    {
        CommandStatus status = CommandStatus::queued;
        std::string order_id;
    };

    // Publishes a multi-node order and tracks completion at its final waypoint; `released_count` limits the released points (nullopt releases all).
    NavigateResult navigate_route(const std::string &name,
                                  const std::vector<RoutePoint> &route,
                                  const std::string &map_id,
                                  std::optional<std::size_t> released_count = std::nullopt);

    // Result of continuing the active order along a replanned route.
    struct ReplanResult
    {
        CommandStatus status = CommandStatus::queued;
        bool stitched = false;
        std::string order_id;
        // Route points the AGV had passed before the new path starts.
        std::size_t consumed = 0;
        // Points of the new path that are already released.
        std::size_t released = 0;
        // Leading points of the new path the order already covers.
        std::size_t leading_dropped = 0;
    };

    // Attach a replanned route to the active order; stitched=false leaves the order untouched.
    ReplanResult replan_route(const std::string &name,
                              const std::vector<RoutePoint> &route,
                              const std::string &map_id,
                              std::optional<std::size_t> released_count = std::nullopt);

    // Extend order `expected_order_id` with a larger released horizon; rejected when it is no longer the tracked order or cannot grow.
    CommandStatus release_more(const std::string &name, const std::string &expected_order_id, std::size_t released_count);

    // Whether the tracked order may still run on the AGV: sent and not yet reported finished.
    bool order_in_progress(const std::string &name) const;

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
    std::string execute_instant_action(const std::string &name, const std::string &action_type,
        const nlohmann::json &parameters = nlohmann::json::object());

    // Requests an immediate state update.
    void request_state(const std::string &name);

    // Sends pending requests, such as factsheetRequest.
    void poll(const std::string &name);

    // Reject hard violations instead of only warning.
    void set_strict_validation(bool strict);

    // Consecutive stale states tolerated before the sender counts as restarted; 0 accepts all.
    void set_stale_state_streak(int streak);

    // How long to wait for a cancelOrder answer and how many times to send it; a zero timeout disables tracking.
    void set_cancel_policy(const vda5050::CancelPolicy &policy);

    // Time limits applied to each AGV: offline, stuck order and factsheet requests.
    void set_link_policy(const LinkPolicy &policy);

    // How far an AGV may stop from each node of the orders it gets.
    void set_node_deviation(const vda5050::NodeDeviation &deviation);

    // Send initPosition in the robot frame and return its action ID, or an empty string on failure.
    std::string init_position(const std::string &name, double x, double y, double theta, const std::string &map_id);

    // State received from the AGV.
    std::optional<RobotData> get_data(const std::string &name);
    // Whether the tracked order reached its final node and settled its actions.
    bool is_command_completed(const std::string &name);

    // Detect an order the AGV has not acknowledged within the link policy's order timeout.
    bool is_order_stuck(const std::string &name) const;
    // Last reported status of an instant action.
    std::optional<std::string> get_action_state(const std::string &name, const std::string &action_id);
    // Status and resultDescription of `action_id` as last reported by the AGV.
    std::optional<std::pair<std::string, std::string>> get_action_result(const std::string &name, const std::string &action_id);
    // Best-known map for `name`, without requiring the AGV to be localized.
    std::optional<std::string> get_known_map(const std::string &name);
    // Whether the robot is connected and has state newer than the link policy's state timeout.
    bool is_online(const std::string &name);

private:
    struct RobotContext
    {
        std::string name;
        std::string manufacturer;
        std::string serial;
        std::string interface_name;
        // Serializes order, order update and cancel commands for this robot.
        std::mutex order_mutex;
        Transform transform;
        // VDA5050 header counters are maintained independently per topic.
        int order_header_id = 0;
        int instant_actions_header_id = 0;
        // Timestamp used to check the factsheet's minimum order interval.
        std::chrono::steady_clock::time_point last_order_time{};
        // Operator speed cap for subsequent orders.
        std::optional<double> operator_speed_limit;
        std::string current_order_id;
        // The AGV reported current_order_id finished at its final node.
        bool order_done = false;
        std::string target_node_id;
        // Actions associated with the tracked order.
        std::vector<std::string> order_action_ids;
        // Update ID of the tracked order, incremented for each horizon extension.
        int order_update_id = 0;
        // Route points passed before the current path began.
        std::size_t route_offset = 0;
        // Robot-frame route data retained to build later horizon updates.
        std::vector<vda5050::RouteWaypoint> current_route;
        std::string current_base_id;
        vda5050::RobotPose current_base;
        std::string current_map_id;
        // Number of route points released in the last published order.
        std::size_t current_released_count = 0;
        std::optional<vda5050::ParsedState> last_state;
        // Header of the last accepted state message.
        vda5050::StateSequence state_sequence;
        // The last cancelOrder sent that the AGV has not answered yet.
        vda5050::CancelTracker cancel;
        // Visualization data is used only to refine pose and velocity.
        std::optional<vda5050::ParsedVisualization> last_visualization;
        std::chrono::steady_clock::time_point last_visualization_time{};
        // Capabilities declared by the AGV.
        std::optional<vda5050::ParsedFactsheet> factsheet;
        // factsheetRequest retry progress.
        int factsheet_requests = 0;
        std::chrono::steady_clock::time_point factsheet_wait_since{};
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
        bool last_paused = false;

        int next_order_header() { return order_header_id++; }
        int next_instant_actions_header() { return instant_actions_header_id++; }
    };

    // Subscribes to a robot's uplink topics.
    void subscribe_robot(const RobotContext &ctx);

    // Offline limit for one AGV, longer than the configured one when its factsheet declares a slower state interval; the caller holds _mutex.
    double state_timeout_for(const RobotContext &ctx) const;

    // The levels of an uplink topic "<interface>/v2/<manufacturer>/<serial>/<leaf>".
    struct TopicLevels
    {
        std::string manufacturer;
        std::string serial;
        std::string leaf;
    };

    // Split a topic into its levels; empty unless it is an uplink topic of this interface.
    std::optional<TopicLevels> parse_topic(const std::string &topic) const;

    // Find the registered robot with the topic's manufacturer and serial; the caller holds _mutex.
    std::shared_ptr<RobotContext> find_by_identity(const TopicLevels &levels) const;

    // Resends an unanswered cancelOrder while the AGV still reports the order.
    void resolve_pending_cancel(const std::string &name);

    // Holds a robot's order lock; empty for an unknown robot. Take it before _mutex.
    struct OrderLock
    {
        std::shared_ptr<RobotContext> robot;
        std::unique_lock<std::mutex> lock;
    };
    OrderLock lock_orders(const std::string &name);

    // Publishes a serialized payload without throwing.
    CommandStatus publish_raw(const std::string &topic, const std::string &payload);

    // Choose a factsheet-compatible blocking type; the caller holds _mutex.
    static std::string blocking_type_for(const RobotContext &ctx, const std::string &action_type, const std::string &preferred);
                                         
    // Report unsupported navigation inputs; the caller holds _mutex.
    void warn_if_unroutable(const RobotContext &ctx, const std::string &dest_node_id,
                            double x, double y, double theta, const std::string &map_id, std::optional<double> speed_limit) const;

    // Report action conflicts; the caller holds _mutex.
    void warn_if_action_conflicts(const RobotContext &ctx, const std::string &action_type, const std::string &blocking_type) const;

    // Convert RMF route points to robot-frame waypoints; caller holds _mutex.
    std::vector<vda5050::RouteWaypoint> to_waypoints(const RobotContext &ctx,  const std::vector<RoutePoint> &route,  const std::string &map_id) const;

    // Log violations; false means do not send. The caller holds _mutex.
    bool order_allowed(RobotContext &ctx, const vda5050::RobotPose &base, const std::vector<vda5050::RouteWaypoint> &route,
                       const std::string &map_id, std::size_t node_count, std::size_t edge_count);

    // Report a map ID mismatch with the AGV; the caller holds _mutex.
    void warn_if_map_mismatch(const RobotContext &ctx, const std::string &order_map_id) const;

    // Keys that change when a state's errors, safety, information, loads or maps change.
    struct StateKeys
    {
        std::string errors;
        std::string safety;
        std::string information;
        std::string loads;
        std::string maps;
    };
    static StateKeys make_state_keys(const vda5050::ParsedState &state);

    // A log message gathered while holding _mutex and written once it is released.
    struct LogLine
    {
        enum class Level
        {
            info,
            warn,
            error
        };
        Level level;
        std::string text;
    };
    void write_log(const std::vector<LogLine> &log) const;

    // Cache a state message and log what changed; the caller holds no lock.
    void update_state(RobotContext &ctx, const nlohmann::json &raw);

    // Record changes in cached operational state into `log`; the caller holds _mutex.
    static void report_state_changes(RobotContext &ctx, const StateKeys &keys, std::vector<LogLine> &log);

    // Logs a dropped stale state unless one was logged recently; the caller holds _mutex.
    void report_stale_state(const RobotContext &ctx, const vda5050::ParsedState &state, std::vector<LogLine> &log);

    // Handle callbacks from the MQTT client.
    void on_connected();
    void on_connection_lost(const std::string &cause);
    void on_error(const std::string &context, const std::string &what);

    // Record a message from a robot that is not registered; the caller holds _mutex.
    void record_unregistered(const TopicLevels &levels, const nlohmann::json &raw);

    // Messages held back since the last log line about `key`; nullopt while the throttle holds this one back.
    std::optional<std::size_t> admit_repeated(const std::string &key);

    rclcpp::Logger _logger;
    std::string _interface_name;
    mqtt::TlsOptions _tls;
    bool _has_credentials;
    mqtt::MqttClient _mqtt_client;

    // Protects robot state shared by MQTT, ROS, and update-loop threads.
    mutable std::mutex _mutex;
    std::map<std::string, std::shared_ptr<RobotContext>> _robots;
    // The same robots keyed by "manufacturer/serial".
    std::unordered_map<std::string, std::shared_ptr<RobotContext>> _robot_index;
    // Unregistered robots keyed by "manufacturer/serial".
    std::map<std::string, DiscoveredRobot> _discovered;
    bool _strict_validation = true;
    int _stale_state_streak = vda5050::StateSequence::kDefaultStreakLimit;
    vda5050::CancelPolicy _cancel_policy;
    LinkPolicy _link_policy;
    vda5050::NodeDeviation _node_deviation;
    // Limits how often a problem that repeats with every message is logged.
    util::LogThrottle _repeat_log;

    // What the message path counts and times; see metrics().
    struct Metrics
    {
        util::Counter rx_state, rx_visualization, rx_connection, rx_factsheet;
        util::Counter bad_payload, bad_topic, unregistered, invalid_state, stale_state, log_suppressed;
        util::Counter published, publish_failed;
        // Time to handle one message, by kind, and how long it waited for _mutex.
        util::Histogram handle_state, handle_other, mutex_wait;
        // Age of a state message from its own timestamp; needs the AGV clock in step with this one.
        util::Histogram state_transit;
    };
    Metrics _metrics;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // CONNECTOR_HPP
