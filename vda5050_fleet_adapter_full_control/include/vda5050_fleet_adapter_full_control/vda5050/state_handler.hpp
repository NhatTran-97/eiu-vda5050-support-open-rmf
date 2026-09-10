#ifndef STATE_HANDLER_HPP
#define STATE_HANDLER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Snapshot of the fields this adapter reads from a VDA5050 'state' message.
// Field-by-field error handling is not uniform:
// - a field absent from the message is left at its default (empty string,
//   false, nullopt).
// - node_states/edge_states/action_states/errors fall back to an empty
//   vector if the field is present but not a JSON array -- no exception.
// - order_update_id accepts any JSON number (int, float, out-of-range) and
//   silently truncates/wraps it into uint32_t rather than rejecting it.
// - every other field throws nlohmann::json::type_error if present with an
//   incompatible JSON type (e.g. a string where a number or boolean is
//   expected).
// Not read-only: nothing prevents a caller from mutating a field after
// construction.
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

    // batteryState.batteryCharge, converted from VDA5050's 0-100 to 0.0-1.0.
    // Not clamped here -- an out-of-range input (e.g. 150) is divided
    // through as-is; the caller must clamp before using it.
    std::optional<double> battery_soc;

    std::string order_id;
    std::optional<std::uint32_t> order_update_id;
    std::string last_node_id;
    bool driving = false;
    bool paused = false;

    std::vector<nlohmann::json> node_states;
    std::vector<nlohmann::json> edge_states;
    std::vector<nlohmann::json> action_states;
    std::vector<nlohmann::json> errors;

    // True when x/y/theta are all present and the AGV reports
    // positionInitialized. Presence only -- does not check that the values
    // are finite or within any sane range.
    bool has_position() const;

    // Route-completion check only, for the given order_id at target_node_id
    // (skipped if empty): true when node/edge states are drained and the
    // AGV isn't driving. Known gaps, deliberate for now:
    // - ignores action_states, so a pending action attached to the final
    //   node would be missed. Harmless today since nothing in this package
    //   attaches actions to nodes yet; revisit if that changes.
    // - treats "field absent from the message" the same as "field present
    //   but empty", so a sparse/malformed state message can look
    //   indistinguishable from a fully drained order.
    // - cannot tell a cancelled order from a completed one -- VDA5050 keeps
    //   the same orderId after a cancelOrder. The caller must clear its own
    //   tracked order id synchronously when it issues a cancel, so this is
    //   never asked about a cancelled order after the fact.
    bool order_finished(const std::string &order_id,
                        const std::string &target_node_id = "") const;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // STATE_HANDLER_HPP
