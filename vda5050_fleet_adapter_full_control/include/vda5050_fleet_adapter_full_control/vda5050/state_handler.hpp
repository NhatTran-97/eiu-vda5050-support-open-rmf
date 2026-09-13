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

// Parsed subset of a VDA5050 state message. Required-field validation is
// performed by Connector before construction; incompatible present values may raise a nlohmann::json type exception.
class ParsedState
{
public:
    ParsedState() = default;
    explicit ParsedState(const nlohmann::json &raw);

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

    // Checks route completion for a tracked order and optional target node. The caller must clear its tracked order after issuing cancelOrder.
    bool order_finished(const std::string &order_id, const std::string &target_node_id = "", const std::vector<std::string> &order_action_ids = {}) const;

    // Returns false when a supplied action is reported in a non-terminal state. Missing and unrelated action states are ignored.
    bool actions_settled(const std::vector<std::string> &action_ids) const;

    // FINISHED / FAILED / RUNNING / ... for one action, or nullopt when the AGV has not reported it.
    std::optional<std::string> action_status(const std::string &action_id) const;
};

// Pose and velocity parsed from a VDA5050 visualization message. Visualization data may refine localization but is not used for order completion.
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
