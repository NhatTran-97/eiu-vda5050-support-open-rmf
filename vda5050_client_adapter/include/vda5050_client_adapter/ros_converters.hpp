#pragma once

/**
 * @file ros_converters.hpp
 * @brief Bidirectional converters between internal VDA5050 C++ structs
 *        and vda5050_msgs ROS2 message types.
 *
 * Naming convention:
 *   ros_from_internal(...)   — internal → ROS2 message
 *   internal_from_ros(...)   — ROS2 message → internal
 */

#include "vda5050_client_adapter/vda5050_types.hpp"

#include <limits>

#include <vda5050_msgs/msg/action.hpp>
#include <vda5050_msgs/msg/action_parameter.hpp>
#include <vda5050_msgs/msg/action_state.hpp>
#include <vda5050_msgs/msg/agv_position.hpp>
#include <vda5050_msgs/msg/battery_state.hpp>
#include <vda5050_msgs/msg/connection.hpp>
#include <vda5050_msgs/msg/control_point.hpp>
#include <vda5050_msgs/msg/corridor.hpp>
#include <vda5050_msgs/msg/edge.hpp>
#include <vda5050_msgs/msg/edge_state.hpp>
#include <vda5050_msgs/msg/error.hpp>
#include <vda5050_msgs/msg/error_reference.hpp>
#include <vda5050_msgs/msg/header.hpp>
#include <vda5050_msgs/msg/info.hpp>
#include <vda5050_msgs/msg/info_reference.hpp>
#include <vda5050_msgs/msg/instant_actions.hpp>
#include <vda5050_msgs/msg/load.hpp>
#include <vda5050_msgs/msg/load_dimensions.hpp>
#include <vda5050_msgs/msg/map_info.hpp>
#include <vda5050_msgs/msg/node.hpp>
#include <vda5050_msgs/msg/node_position.hpp>
#include <vda5050_msgs/msg/node_state.hpp>
#include <vda5050_msgs/msg/order.hpp>
#include <vda5050_msgs/msg/safety_state.hpp>
#include <vda5050_msgs/msg/state.hpp>
#include <vda5050_msgs/msg/trajectory.hpp>
#include <vda5050_msgs/msg/velocity.hpp>
#include <vda5050_msgs/msg/visualization.hpp>

namespace vda5050_adapter {

// ─────────────────────────────────────────────────────────────────────────────
// String ↔ Enum helpers
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050::OperatingMode
internal_from_ros_operating_mode(const std::string& s) {
  if (s == "SEMIAUTOMATIC") return vda5050::OperatingMode::SEMIAUTOMATIC;
  if (s == "MANUAL")        return vda5050::OperatingMode::MANUAL;
  if (s == "SERVICE")       return vda5050::OperatingMode::SERVICE;
  if (s == "TEACHIN")       return vda5050::OperatingMode::TEACHIN;
  return vda5050::OperatingMode::AUTOMATIC;
}

inline vda5050::ActionStatus
internal_from_ros_action_status(const std::string& s) {
  if (s == "INITIALIZING") return vda5050::ActionStatus::INITIALIZING;
  if (s == "RUNNING")      return vda5050::ActionStatus::RUNNING;
  if (s == "PAUSED")       return vda5050::ActionStatus::PAUSED;
  if (s == "FINISHED")     return vda5050::ActionStatus::FINISHED;
  if (s == "FAILED")       return vda5050::ActionStatus::FAILED;
  return vda5050::ActionStatus::WAITING;
}

inline std::string ros_from_action_status(vda5050::ActionStatus s) {
  switch (s) {
    case vda5050::ActionStatus::INITIALIZING: return "INITIALIZING";
    case vda5050::ActionStatus::RUNNING:      return "RUNNING";
    case vda5050::ActionStatus::PAUSED:       return "PAUSED";
    case vda5050::ActionStatus::FINISHED:     return "FINISHED";
    case vda5050::ActionStatus::FAILED:       return "FAILED";
    default:                                  return "WAITING";
  }
}

inline std::string ros_from_blocking_type(vda5050::BlockingType b) {
  switch (b) {
    case vda5050::BlockingType::SOFT: return "SOFT";
    case vda5050::BlockingType::HARD: return "HARD";
    default:                          return "NONE";
  }
}

inline vda5050::BlockingType internal_from_ros_blocking_type(const std::string& s) {
  if (s == "SOFT") return vda5050::BlockingType::SOFT;
  if (s == "HARD") return vda5050::BlockingType::HARD;
  return vda5050::BlockingType::NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Header
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::Header
ros_from_internal(const vda5050::Header& h) {
  vda5050_msgs::msg::Header out;
  out.header_id    = h.header_id;
  out.timestamp    = h.timestamp;
  out.version      = h.version;
  out.manufacturer = h.manufacturer;
  out.serial_number = h.serial_number;
  return out;
}

inline vda5050::Header
internal_from_ros(const vda5050_msgs::msg::Header& h) {
  vda5050::Header out;
  out.header_id    = h.header_id;
  out.timestamp    = h.timestamp;
  out.version      = h.version;
  out.manufacturer = h.manufacturer;
  out.serial_number = h.serial_number;
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// ActionParameter / Action
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::ActionParameter
ros_from_internal(const vda5050::ActionParameter& p) {
  vda5050_msgs::msg::ActionParameter out;
  out.key   = p.key;
  out.value = p.value;
  return out;
}

inline vda5050::ActionParameter
internal_from_ros(const vda5050_msgs::msg::ActionParameter& p) {
  return {p.key, p.value};
}

inline vda5050_msgs::msg::Action
ros_from_internal(const vda5050::Action& a) {
  vda5050_msgs::msg::Action out;
  out.action_type        = a.action_type;
  out.action_id          = a.action_id;
  out.action_description = a.action_description;
  out.blocking_type      = ros_from_blocking_type(a.blocking_type);
  for (const auto& p : a.action_parameters) out.action_parameters.push_back(ros_from_internal(p));
  return out;
}

inline vda5050::Action
internal_from_ros(const vda5050_msgs::msg::Action& a) {
  vda5050::Action out;
  out.action_type        = a.action_type;
  out.action_id          = a.action_id;
  out.action_description = a.action_description;
  out.blocking_type      = internal_from_ros_blocking_type(a.blocking_type);
  for (const auto& p : a.action_parameters) out.action_parameters.push_back(internal_from_ros(p));
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// NodePosition
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::NodePosition
ros_from_internal(const vda5050::NodePosition& p) {
  vda5050_msgs::msg::NodePosition out;
  out.x                      = p.x;
  out.y                      = p.y;
  out.theta                  = p.theta;
  out.theta_set              = p.theta_set;
  out.allowed_deviation_xy   = p.allowed_deviation_xy;
  out.allowed_deviation_theta= p.allowed_deviation_theta;
  out.map_id                 = p.map_id;
  out.map_description        = p.map_description;
  return out;
}

inline vda5050::NodePosition
internal_from_ros(const vda5050_msgs::msg::NodePosition& p) {
  vda5050::NodePosition out;
  out.x                       = p.x;
  out.y                       = p.y;
  out.theta                   = p.theta;
  out.theta_set               = p.theta_set;
  out.allowed_deviation_xy    = p.allowed_deviation_xy;
  out.allowed_deviation_theta = p.allowed_deviation_theta;
  out.map_id                  = p.map_id;
  out.map_description         = p.map_description;
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Trajectory / Corridor
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::ControlPoint
ros_from_internal(const vda5050::ControlPoint& cp) {
  vda5050_msgs::msg::ControlPoint out;
  out.x = cp.x;  out.y = cp.y;  out.weight = cp.weight;
  return out;
}

inline vda5050_msgs::msg::Trajectory
ros_from_internal(const vda5050::Trajectory& t) {
  vda5050_msgs::msg::Trajectory out;
  out.degree      = t.degree;
  out.knot_vector = t.knot_vector;
  for (const auto& cp : t.control_points) out.control_points.push_back(ros_from_internal(cp));
  return out;
}

inline vda5050_msgs::msg::Corridor
ros_from_internal(const vda5050::Corridor& c) {
  vda5050_msgs::msg::Corridor out;
  out.left_width          = c.left_width;
  out.right_width         = c.right_width;
  out.corridor_ref_point  = c.corridor_ref_point;
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Node / Edge
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::Node
ros_from_internal(const vda5050::Node& n) {
  vda5050_msgs::msg::Node out;
  out.node_id          = n.node_id;
  out.sequence_id      = n.sequence_id;
  out.node_description = n.node_description;
  out.released         = n.released;
  out.node_position_set = n.node_position.has_value();
  if (n.node_position.has_value()) out.node_position = ros_from_internal(n.node_position.value());
  for (const auto& a : n.actions) out.actions.push_back(ros_from_internal(a));
  return out;
}

inline vda5050_msgs::msg::Edge
ros_from_internal(const vda5050::Edge& e) {
  vda5050_msgs::msg::Edge out;
  out.edge_id          = e.edge_id;
  out.sequence_id      = e.sequence_id;
  out.edge_description = e.edge_description;
  out.released         = e.released;
  out.start_node_id    = e.start_node_id;
  out.end_node_id      = e.end_node_id;
  out.max_speed        = e.max_speed;
  out.max_height       = e.max_height;
  out.min_height       = e.min_height;
  out.orientation      = e.orientation.value_or(
    std::numeric_limits<double>::quiet_NaN());
  out.orientation_type = e.orientation_type;
  out.direction        = e.direction;
  out.rotation_allowed     = e.rotation_allowed.value_or(false);
  out.rotation_allowed_set = e.rotation_allowed.has_value();
  out.max_rotation_speed   = e.max_rotation_speed;
  out.length           = e.length;
  out.trajectory_set   = e.trajectory.has_value();
  if (e.trajectory.has_value()) out.trajectory = ros_from_internal(e.trajectory.value());
  out.corridor_set     = e.corridor.has_value();
  if (e.corridor.has_value()) out.corridor = ros_from_internal(e.corridor.value());
  for (const auto& a : e.actions) out.actions.push_back(ros_from_internal(a));
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Order / InstantActions
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::Order
ros_from_internal(const vda5050::Order& o) {
  vda5050_msgs::msg::Order out;
  out.header          = ros_from_internal(o.header);
  out.order_id        = o.order_id;
  out.order_update_id = o.order_update_id;
  out.zone_set_id     = o.zone_set_id;
  for (const auto& n : o.nodes) out.nodes.push_back(ros_from_internal(n));
  for (const auto& e : o.edges) out.edges.push_back(ros_from_internal(e));
  return out;
}

inline vda5050_msgs::msg::InstantActions
ros_from_internal(const vda5050::InstantActions& ia) {
  vda5050_msgs::msg::InstantActions out;
  out.header = ros_from_internal(ia.header);
  for (const auto& a : ia.actions) out.actions.push_back(ros_from_internal(a));
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// AgvPosition / Velocity / BatteryState / SafetyState
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::AgvPosition
ros_from_internal(const vda5050::AgvPosition& p) {
  vda5050_msgs::msg::AgvPosition out;
  out.position_initialized = p.position_initialized;
  out.localization_score   = p.localization_score;
  out.deviation_range      = p.deviation_range;
  out.x                    = p.x;
  out.y                    = p.y;
  out.theta                = p.theta;
  out.map_id               = p.map_id;
  out.map_description      = p.map_description;
  return out;
}

inline vda5050::AgvPosition
internal_from_ros(const vda5050_msgs::msg::AgvPosition& p) {
  vda5050::AgvPosition out;
  out.position_initialized = p.position_initialized;
  out.localization_score   = p.localization_score;
  out.deviation_range      = p.deviation_range;
  out.x                    = p.x;
  out.y                    = p.y;
  out.theta                = p.theta;
  out.map_id               = p.map_id;
  out.map_description      = p.map_description;
  return out;
}

inline vda5050_msgs::msg::Velocity
ros_from_internal(const vda5050::Velocity& v) {
  vda5050_msgs::msg::Velocity out;
  out.vx = v.vx; out.vy = v.vy; out.omega = v.omega;
  return out;
}

inline vda5050::Velocity
internal_from_ros(const vda5050_msgs::msg::Velocity& v) {
  return {v.vx, v.vy, v.omega};
}

inline vda5050_msgs::msg::BatteryState
ros_from_internal(const vda5050::BatteryState& b) {
  vda5050_msgs::msg::BatteryState out;
  out.battery_charge  = b.battery_charge;
  out.battery_voltage = b.battery_voltage;
  out.battery_health  = b.battery_health;
  out.charging        = b.charging;
  out.reach           = b.reach;
  return out;
}

inline vda5050::BatteryState
internal_from_ros(const vda5050_msgs::msg::BatteryState& b) {
  vda5050::BatteryState out;
  out.battery_charge  = b.battery_charge;
  out.battery_voltage = b.battery_voltage;
  out.battery_health  = b.battery_health;
  out.charging        = b.charging;
  out.reach           = b.reach;
  return out;
}

inline vda5050::SafetyState
internal_from_ros(const vda5050_msgs::msg::SafetyState& s) {
  vda5050::SafetyState out;
  out.field_violation = s.field_violation;
  if      (s.e_stop == "AUTOACK") out.e_stop = vda5050::EStop::AUTOACK;
  else if (s.e_stop == "MANUAL")  out.e_stop = vda5050::EStop::MANUAL;
  else if (s.e_stop == "REMOTE")  out.e_stop = vda5050::EStop::REMOTE;
  else                            out.e_stop = vda5050::EStop::NONE;
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Error / Info
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::Error
ros_from_internal(const vda5050::Error& e) {
  vda5050_msgs::msg::Error out;
  out.error_type        = e.error_type;
  out.error_description = e.error_description;
  out.error_hint        = e.error_hint;
  out.error_level       = (e.error_level == vda5050::ErrorLevel::FATAL) ? "FATAL" : "WARNING";
  for (const auto& r : e.error_references) {
    vda5050_msgs::msg::ErrorReference ref;
    ref.reference_key   = r.reference_key;
    ref.reference_value = r.reference_value;
    out.error_references.push_back(ref);
  }
  return out;
}

inline vda5050::Error
internal_from_ros(const vda5050_msgs::msg::Error& e) {
  vda5050::Error out;
  out.error_type        = e.error_type;
  out.error_description = e.error_description;
  out.error_hint        = e.error_hint;
  out.error_level       = (e.error_level == "FATAL")
                          ? vda5050::ErrorLevel::FATAL
                          : vda5050::ErrorLevel::WARNING;
  for (const auto& r : e.error_references) {
    out.error_references.push_back({r.reference_key, r.reference_value});
  }
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load
// ─────────────────────────────────────────────────────────────────────────────

inline vda5050_msgs::msg::Load
ros_from_internal(const vda5050::Load& l) {
  vda5050_msgs::msg::Load out;
  out.load_id       = l.load_id;
  out.load_type     = l.load_type;
  out.load_position = l.load_position;
  out.weight        = l.weight;
  out.bounding_box_set = l.bounding_box_reference.has_value();
  if (l.bounding_box_reference.has_value()) {
    out.bounding_box_reference.x     = l.bounding_box_reference->x;
    out.bounding_box_reference.y     = l.bounding_box_reference->y;
    out.bounding_box_reference.z     = l.bounding_box_reference->z;
    out.bounding_box_reference.theta = l.bounding_box_reference->theta;
  }
  out.dimensions_set = l.load_dimensions.has_value();
  if (l.load_dimensions.has_value()) {
    out.load_dimensions.length = l.load_dimensions->length;
    out.load_dimensions.width  = l.load_dimensions->width;
    out.load_dimensions.height = l.load_dimensions->height;
  }
  return out;
}

inline vda5050::Load
internal_from_ros(const vda5050_msgs::msg::Load& l) {
  vda5050::Load out;
  out.load_id       = l.load_id;
  out.load_type     = l.load_type;
  out.load_position = l.load_position;
  out.weight        = l.weight;
  if (l.bounding_box_set) {
    vda5050::BoundingBoxReference bbr;
    bbr.x     = l.bounding_box_reference.x;
    bbr.y     = l.bounding_box_reference.y;
    bbr.z     = l.bounding_box_reference.z;
    bbr.theta = l.bounding_box_reference.theta;
    out.bounding_box_reference = bbr;
  }
  if (l.dimensions_set) {
    vda5050::LoadDimensions ld;
    ld.length = l.load_dimensions.length;
    ld.width  = l.load_dimensions.width;
    ld.height = l.load_dimensions.height;
    out.load_dimensions = ld;
  }
  return out;
}

}  // namespace vda5050_adapter
