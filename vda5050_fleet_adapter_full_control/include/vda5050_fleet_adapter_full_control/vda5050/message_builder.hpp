#ifndef MESSAGE_BUILDER_HPP
#define MESSAGE_BUILDER_HPP

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace vda5050_fleet_adapter_full_control::vda5050 {

inline constexpr const char* VERSION = "2.1.0";

inline constexpr const char* TOPIC_ORDER = "order";
inline constexpr const char* TOPIC_INSTANT_ACTIONS = "instantActions";
inline constexpr const char* TOPIC_STATE = "state";
inline constexpr const char* TOPIC_CONNECTION = "connection";
inline constexpr const char* TOPIC_VISUALIZATION = "visualization";
inline constexpr const char* TOPIC_FACTSHEET = "factsheet";



// VDA5050 timestamp: ISO8601 UTC, millisecond precision, 'Z' suffix (ex. 2026-09-08 T 12:30:45.123 Z)

std::string now_iso();

std::string topic(const std::string &interface_name,
                  const std::string &manufacturer,
                  const std::string &serial,
                  const std::string &leaf);


/*
 * Preconditions below are NOT validated by these functions -- the caller (order_handler / connector) is responsible for passing sane values:
 * x/y/theta finite, allowed_deviation_xy >= 0, allowed_deviation_theta within VDA5050's documented [0, pi] bound.
 */

nlohmann::json make_node(const std::string &node_id, int sequence_id,
                        double x, double y, double theta,
                        const std::string &map_id, bool released = true,
                        double allowed_deviation_xy = 0.5,
                        double allowed_deviation_theta = 3.14);

nlohmann::json make_edge(const std::string &edge_id, int sequence_id,
                        const std::string &start_node_id, const std::string &end_node_id,
                        bool released = true, std::optional<double> max_speed = std::nullopt);

// header_id must be >= 0 -- not validated here.
nlohmann::json make_order(int header_id, const std::string &manufacturer, const std::string &serial,
                         const nlohmann::json &nodes, const nlohmann::json &edges,
                          const std::string &order_id = "", int order_update_id = 0);


// blocking_type must be one of NONE | SOFT | HARD -- not validated here.
nlohmann::json make_action(const std::string &action_type, const std::string &blocking_type = "HARD",
                           const std::string &action_id = "", const std::vector<std::pair<std::string, std::string>> &parameters = {});

nlohmann::json make_instant_actions(int header_id,
                                    const std::string &manufacturer,
                                    const std::string &serial, const nlohmann::json &actions);

nlohmann::json cancel_order_action(const std::string &action_id = "");

std::string make_uuid();

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // MESSAGE_BUILDER_HPP
