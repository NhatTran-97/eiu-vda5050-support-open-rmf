#ifndef MESSAGE_BUILDER_HPP
#define MESSAGE_BUILDER_HPP

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

inline constexpr const char *VERSION = "2.1.0";

inline constexpr const char *TOPIC_ORDER = "order";
inline constexpr const char *TOPIC_INSTANT_ACTIONS = "instantActions";
inline constexpr const char *TOPIC_STATE = "state";
inline constexpr const char *TOPIC_CONNECTION = "connection";
inline constexpr const char *TOPIC_VISUALIZATION = "visualization";
inline constexpr const char *TOPIC_FACTSHEET = "factsheet";

// Returns an ISO 8601 UTC timestamp with millisecond precision.
std::string now_iso();

std::string topic(const std::string &interface_name,
                  const std::string &manufacturer,
                  const std::string &serial,
                  const std::string &leaf);
// Inputs are serialized without validation. Callers must provide finite poses
// and deviations within the VDA5050 limits.
nlohmann::json make_node(const std::string &node_id, int sequence_id,
                        double x, double y, double theta,
                        const std::string &map_id, bool released = true,
                        double allowed_deviation_xy = 0.5,
                        double allowed_deviation_theta = 3.14);

nlohmann::json make_edge(const std::string &edge_id, int sequence_id,
                        const std::string &start_node_id, const std::string &end_node_id,
                        bool released = true, std::optional<double> max_speed = std::nullopt);

// Builds an order envelope from prevalidated node and edge arrays.
nlohmann::json make_order(int header_id, const std::string &manufacturer, const std::string &serial,
                         const nlohmann::json &nodes, const nlohmann::json &edges,
                          const std::string &order_id = "", int order_update_id = 0);
// `blocking_type` must be NONE, SOFT, or HARD. Parameter value types are
// preserved when converted to VDA5050 actionParameters.
nlohmann::json make_action(const std::string &action_type, const std::string &blocking_type = "HARD",
                           const std::string &action_id = "",
                           const nlohmann::json &parameters = nlohmann::json::object());

nlohmann::json make_instant_actions(int header_id,
                                    const std::string &manufacturer,
                                    const std::string &serial, const nlohmann::json &actions);

nlohmann::json cancel_order_action(const std::string &action_id = "",
                                   const std::string &blocking_type = "HARD");

std::string make_uuid();

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // MESSAGE_BUILDER_HPP
