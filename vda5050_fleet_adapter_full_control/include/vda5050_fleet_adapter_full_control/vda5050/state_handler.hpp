#ifndef STATE_HANDLER_HPP
#define STATE_HANDLER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Returns true for terminal VDA5050 action states: FINISHED and FAILED.
bool is_terminal_action_status(const std::string &status);

// Parse an ISO 8601 UTC timestamp such as "2026-09-20T10:00:00.123Z" into milliseconds since the epoch.
// Returns nullopt for any other form.
std::optional<std::int64_t> parse_timestamp_ms(const std::string &text);

// Velocity reported in the AGV frame.
struct Velocity
{
    double vx = 0.0;
    double vy = 0.0;
    double omega = 0.0;

    // Translational speed in m/s, ignoring rotation.
    double speed() const;
};

// Parsed state.safetyState fields.
struct SafetyState
{
    // AUTOACK | MANUAL | REMOTE | NONE.
    std::string e_stop = "NONE";
    bool field_violation = false;

    // True while an emergency stop or protective-field violation is active.
    bool triggered() const;
};

// Parsed VDA5050 state used for pose, battery, safety, and order tracking.
class ParsedState
{
public:
    ParsedState() = default;
    explicit ParsedState(const nlohmann::json &raw);

    // headerId and timestamp (epoch milliseconds) of the message, when present and well formed.
    std::optional<std::uint32_t> header_id;
    std::optional<std::int64_t> timestamp_ms;

    // agvPosition
    std::optional<double> x;
    std::optional<double> y;
    std::optional<double> theta;
    std::string map_id;
    bool position_initialized = false;
    // agvPosition.localizationScore in the range [0, 1], when reported.
    std::optional<double> localization_score;

    // batteryState.batteryCharge divided by 100 to obtain a fractional value.
    std::optional<double> battery_soc;
    bool charging = false;

    std::string order_id;
    std::optional<std::uint32_t> order_update_id;
    std::string zone_set_id;
    std::string last_node_id;
    std::optional<std::uint32_t> last_node_sequence_id;
    bool driving = false;
    bool paused = false;
    // Indicates that the AGV requests additional released horizon.
    bool new_base_request = false;
    // Metres driven since last_node_id was reached.
    std::optional<double> distance_since_last_node;

    // AUTOMATIC | SEMIAUTOMATIC | MANUAL | SERVICE | TEACHIN.
    std::string operating_mode = "AUTOMATIC";

    std::optional<Velocity> velocity;
    SafetyState safety_state;

    std::vector<nlohmann::json> node_states;
    std::vector<nlohmann::json> edge_states;
    std::vector<nlohmann::json> action_states;
    std::vector<nlohmann::json> errors;
    std::vector<nlohmann::json> information;
    std::vector<nlohmann::json> loads;
    std::vector<nlohmann::json> maps;

    // True when a complete, initialized AGV position is available.
    bool has_position() const;

    // True when the operating mode accepts master-control orders.
    bool operable() const;

    // Returns the first FATAL error type, or an empty string when none exists.
    std::string first_fatal_error() const;

    // Check whether the tracked order reached its target node.
    bool order_finished(const std::string &expected_order_id, const std::string &target_node_id = "", const std::vector<std::string> &order_action_ids = {}) const;

    // Check whether the supplied actions all reached terminal states.
    bool actions_settled(const std::vector<std::string> &action_ids) const;

    // Read an action's reported status, or nullopt before it appears.
    std::optional<std::string> action_status(const std::string &action_id) const;
};

// Pose and velocity from visualization messages, used to refine localization.
class ParsedVisualization
{
public:
    ParsedVisualization() = default;
    explicit ParsedVisualization(const nlohmann::json &raw);

    std::optional<double> x;
    std::optional<double> y;
    std::optional<double> theta;
    std::string map_id;
    bool position_initialized = false;
    std::optional<Velocity> velocity;

    bool has_position() const;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // STATE_HANDLER_HPP
