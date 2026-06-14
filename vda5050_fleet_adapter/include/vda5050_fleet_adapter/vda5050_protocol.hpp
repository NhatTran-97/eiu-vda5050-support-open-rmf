#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

/// Transport-agnostic VDA5050 v2.x message helpers (build + parse). All wire
/// format knowledge lives here so the rest of the adapter stays clean.
/// Ported from the reference vda5050_messages.py.
namespace vda5050_fleet_adapter::protocol {

inline constexpr const char* VERSION = "2.1.0";

// Topic leaf names
inline constexpr const char* TOPIC_ORDER = "order";
inline constexpr const char* TOPIC_INSTANT_ACTIONS = "instantActions";
inline constexpr const char* TOPIC_STATE = "state";
inline constexpr const char* TOPIC_CONNECTION = "connection";
inline constexpr const char* TOPIC_VISUALIZATION = "visualization";
inline constexpr const char* TOPIC_FACTSHEET = "factsheet";

/// VDA5050 timestamp: ISO8601 UTC, millisecond precision, 'Z' suffix.
std::string now_iso();

/// Full VDA5050 topic: "<interface>/v2/<manufacturer>/<serial>/<leaf>".
std::string topic(const std::string& interface_name,
                  const std::string& manufacturer,
                  const std::string& serial,
                  const std::string& leaf);

/// Build one order node (sequenceId must be EVEN).
nlohmann::json make_node(const std::string& node_id, int sequence_id,
                         double x, double y, double theta,
                         const std::string& map_id,
                         bool released = true,
                         double allowed_deviation_xy = 0.5,
                         double allowed_deviation_theta = 3.15);

/// Build one order edge (sequenceId must be ODD).
nlohmann::json make_edge(const std::string& edge_id, int sequence_id,
                         const std::string& start_node_id,
                         const std::string& end_node_id,
                         bool released = true,
                         std::optional<double> max_speed = std::nullopt);

/// Assemble a complete VDA5050 'order' message. Empty order_id -> a fresh UUID.
nlohmann::json make_order(int header_id,
                          const std::string& manufacturer,
                          const std::string& serial,
                          const nlohmann::json& nodes,
                          const nlohmann::json& edges,
                          const std::string& order_id = "",
                          int order_update_id = 0);

/// Build a single VDA5050 action. blocking_type: NONE | SOFT | HARD.
/// Empty action_id -> a fresh UUID. `parameters` is a flat key->string map.
nlohmann::json make_action(
  const std::string& action_type,
  const std::string& blocking_type = "HARD",
  const std::string& action_id = "",
  const std::vector<std::pair<std::string, std::string>>& parameters = {});

/// Assemble a VDA5050 'instantActions' message.
nlohmann::json make_instant_actions(int header_id,
                                    const std::string& manufacturer,
                                    const std::string& serial,
                                    const nlohmann::json& actions);

/// Standard instant action to abort the current order.
nlohmann::json cancel_order_action(const std::string& action_id = "");

/// Generate a UUID-v4 string (used for orderId / actionId).
std::string make_uuid();

// ─── State parsing (AGV -> RMF) ───────────────────────────────────────────────

/// Lightweight, read-only view over an incoming VDA5050 'state' message.
class ParsedState
{
public:
  ParsedState() = default;
  explicit ParsedState(const nlohmann::json& raw);

  // agvPosition
  std::optional<double> x;
  std::optional<double> y;
  std::optional<double> theta;
  std::string map_id;
  bool position_initialized = false;

  std::optional<double> battery_soc;  // 0.0–1.0 (from VDA5050 0–100 %)

  std::string order_id;
  std::optional<int> order_update_id;
  std::string last_node_id;
  bool driving = false;
  bool paused = false;

  std::vector<nlohmann::json> node_states;
  std::vector<nlohmann::json> edge_states;
  std::vector<nlohmann::json> action_states;
  std::vector<nlohmann::json> errors;

  /// True when a valid, initialized AGV position is present.
  bool has_position() const;

  /// True when `order_id` has been fully executed (no pending node/edge states,
  /// not driving). A different reported orderId means "not finished".
  bool order_finished(const std::string& order_id) const;
};

}  // namespace vda5050_fleet_adapter::protocol
