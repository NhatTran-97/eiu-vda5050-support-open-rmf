#pragma once

/**
 * @file json_converter.hpp
 * @brief nlohmann/json serialization and deserialization for all VDA5050 v2.1.0 types.
 *
 * Usage:
 *   // Deserialize
 *   auto j = nlohmann::json::parse(payload);
 *   vda5050::Order order = j.get<vda5050::Order>();
 *
 *   // Serialize
 *   nlohmann::json j = state;
 *   std::string payload = j.dump();
 */

#include <optional>

#include <nlohmann/json.hpp>
#include "vda5050_client_adapter/vda5050_types.hpp"

// Helper macro: read optional field (skip if absent or null)
#define VDA_FROM_OPT(j, key, field)                       \
  if ((j).contains(#key) && !(j)[#key].is_null()) {       \
    (field) = (j)[#key].get<decltype(field)>();            \
  }

// Helper macro: write optional field (skip if not set)
#define VDA_TO_OPT(j, key, field)  \
  if ((field).has_value()) {       \
    (j)[#key] = (field).value();   \
  }

namespace nlohmann {

// ─────────────────────────────────────────────────────────────────────────────
// Generic std::optional<T> support
// ─────────────────────────────────────────────────────────────────────────────
// Enables .get<std::optional<T>>() and implicit j = optional_value serialization.

template <typename T>
struct adl_serializer<std::optional<T>> {
  static void to_json(json& j, const std::optional<T>& opt) {
    if (opt.has_value()) j = opt.value();
    else                 j = nullptr;
  }
  static void from_json(const json& j, std::optional<T>& opt) {
    if (j.is_null()) opt = std::nullopt;
    else             opt = j.get<T>();
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Enum conversions
// ─────────────────────────────────────────────────────────────────────────────

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::BlockingType, {
  {vda5050::BlockingType::NONE, "NONE"},
  {vda5050::BlockingType::SOFT, "SOFT"},
  {vda5050::BlockingType::HARD, "HARD"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::ActionStatus, {
  {vda5050::ActionStatus::WAITING,       "WAITING"},
  {vda5050::ActionStatus::INITIALIZING,  "INITIALIZING"},
  {vda5050::ActionStatus::RUNNING,       "RUNNING"},
  {vda5050::ActionStatus::PAUSED,        "PAUSED"},
  {vda5050::ActionStatus::FINISHED,      "FINISHED"},
  {vda5050::ActionStatus::FAILED,        "FAILED"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::EStop, {
  {vda5050::EStop::AUTOACK, "AUTOACK"},
  {vda5050::EStop::MANUAL,  "MANUAL"},
  {vda5050::EStop::REMOTE,  "REMOTE"},
  {vda5050::EStop::NONE,    "NONE"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::OperatingMode, {
  {vda5050::OperatingMode::AUTOMATIC,    "AUTOMATIC"},
  {vda5050::OperatingMode::SEMIAUTOMATIC,"SEMIAUTOMATIC"},
  {vda5050::OperatingMode::MANUAL,       "MANUAL"},
  {vda5050::OperatingMode::SERVICE,      "SERVICE"},
  {vda5050::OperatingMode::TEACHIN,      "TEACHIN"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::ConnectionState, {
  {vda5050::ConnectionState::ONLINE,            "ONLINE"},
  {vda5050::ConnectionState::OFFLINE,           "OFFLINE"},
  {vda5050::ConnectionState::CONNECTIONBROKEN,  "CONNECTIONBROKEN"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::ErrorLevel, {
  {vda5050::ErrorLevel::WARNING, "WARNING"},
  {vda5050::ErrorLevel::FATAL,   "FATAL"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::InfoLevel, {
  {vda5050::InfoLevel::DEBUG, "DEBUG"},
  {vda5050::InfoLevel::INFO,  "INFO"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(vda5050::MapStatus, {
  {vda5050::MapStatus::ENABLED,  "ENABLED"},
  {vda5050::MapStatus::DISABLED, "DISABLED"},
})

// ─────────────────────────────────────────────────────────────────────────────
// Header
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::Header> {
  static void to_json(json& j, const vda5050::Header& h) {
    j = {
      {"headerId",     h.header_id},
      {"timestamp",    h.timestamp},
      {"version",      h.version},
      {"manufacturer", h.manufacturer},
      {"serialNumber", h.serial_number},
    };
  }
  static void from_json(const json& j, vda5050::Header& h) {
    j.at("headerId").get_to(h.header_id);
    j.at("timestamp").get_to(h.timestamp);
    j.at("version").get_to(h.version);
    j.at("manufacturer").get_to(h.manufacturer);
    j.at("serialNumber").get_to(h.serial_number);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Actions
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::ActionParameter> {
  static void to_json(json& j, const vda5050::ActionParameter& p) {
    j = {{"key", p.key}, {"value", p.value}};
  }
  static void from_json(const json& j, vda5050::ActionParameter& p) {
    j.at("key").get_to(p.key);
    // value may be any JSON type — store as string representation
    if (j.at("value").is_string()) {
      p.value = j.at("value").get<std::string>();
    } else {
      p.value = j.at("value").dump();
    }
  }
};

template<>
struct adl_serializer<vda5050::Action> {
  static void to_json(json& j, const vda5050::Action& a) {
    j = {
      {"actionType",        a.action_type},
      {"actionId",          a.action_id},
      {"blockingType",      a.blocking_type},
      {"actionParameters",  a.action_parameters},
    };
    if (!a.action_description.empty()) j["actionDescription"] = a.action_description;
  }
  static void from_json(const json& j, vda5050::Action& a) {
    j.at("actionType").get_to(a.action_type);
    j.at("actionId").get_to(a.action_id);
    j.at("blockingType").get_to(a.blocking_type);
    if (j.contains("actionDescription")) j["actionDescription"].get_to(a.action_description);
    if (j.contains("actionParameters"))  j["actionParameters"].get_to(a.action_parameters);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// NodePosition
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::NodePosition> {
  static void to_json(json& j, const vda5050::NodePosition& p) {
    j = {
      {"x",     p.x},
      {"y",     p.y},
      {"mapId", p.map_id},
    };
    if (p.theta_set)                          j["theta"]                 = p.theta;
    if (p.allowed_deviation_xy != 0.0)        j["allowedDeviationXY"]    = p.allowed_deviation_xy;
    if (p.allowed_deviation_theta != 0.0)     j["allowedDeviationTheta"] = p.allowed_deviation_theta;
    if (!p.map_description.empty())           j["mapDescription"]        = p.map_description;
  }
  static void from_json(const json& j, vda5050::NodePosition& p) {
    j.at("x").get_to(p.x);
    j.at("y").get_to(p.y);
    j.at("mapId").get_to(p.map_id);
    if (j.contains("theta")) { j["theta"].get_to(p.theta); p.theta_set = true; }
    if (j.contains("allowedDeviationXY"))    j["allowedDeviationXY"].get_to(p.allowed_deviation_xy);
    if (j.contains("allowedDeviationTheta")) j["allowedDeviationTheta"].get_to(p.allowed_deviation_theta);
    if (j.contains("mapDescription"))        j["mapDescription"].get_to(p.map_description);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Trajectory / Corridor
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::ControlPoint> {
  static void to_json(json& j, const vda5050::ControlPoint& cp) {
    j = {{"x", cp.x}, {"y", cp.y}, {"weight", cp.weight}};
  }
  static void from_json(const json& j, vda5050::ControlPoint& cp) {
    j.at("x").get_to(cp.x);
    j.at("y").get_to(cp.y);
    if (j.contains("weight")) j["weight"].get_to(cp.weight);
  }
};

template<>
struct adl_serializer<vda5050::Trajectory> {
  static void to_json(json& j, const vda5050::Trajectory& t) {
    j = {
      {"degree",        t.degree},
      {"knotVector",    t.knot_vector},
      {"controlPoints", t.control_points},
    };
  }
  static void from_json(const json& j, vda5050::Trajectory& t) {
    j.at("degree").get_to(t.degree);
    j.at("knotVector").get_to(t.knot_vector);
    j.at("controlPoints").get_to(t.control_points);
  }
};

template<>
struct adl_serializer<vda5050::Corridor> {
  static void to_json(json& j, const vda5050::Corridor& c) {
    j = {
      {"leftWidth",         c.left_width},
      {"rightWidth",        c.right_width},
      {"corridorRefPoint",  c.corridor_ref_point},
    };
  }
  static void from_json(const json& j, vda5050::Corridor& c) {
    j.at("leftWidth").get_to(c.left_width);
    j.at("rightWidth").get_to(c.right_width);
    if (j.contains("corridorRefPoint")) j["corridorRefPoint"].get_to(c.corridor_ref_point);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Node / Edge
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::Node> {
  static void to_json(json& j, const vda5050::Node& n) {
    j = {
      {"nodeId",      n.node_id},
      {"sequenceId",  n.sequence_id},
      {"released",    n.released},
      {"actions",     n.actions},
    };
    if (!n.node_description.empty()) j["nodeDescription"] = n.node_description;
    if (n.node_position.has_value()) j["nodePosition"]    = n.node_position.value();
  }
  static void from_json(const json& j, vda5050::Node& n) {
    j.at("nodeId").get_to(n.node_id);
    j.at("sequenceId").get_to(n.sequence_id);
    j.at("released").get_to(n.released);
    j.at("actions").get_to(n.actions);
    if (j.contains("nodeDescription")) j["nodeDescription"].get_to(n.node_description);
    if (j.contains("nodePosition") && !j["nodePosition"].is_null()) {
      n.node_position = j["nodePosition"].get<vda5050::NodePosition>();
    }
  }
};

template<>
struct adl_serializer<vda5050::Edge> {
  static void to_json(json& j, const vda5050::Edge& e) {
    j = {
      {"edgeId",       e.edge_id},
      {"sequenceId",   e.sequence_id},
      {"released",     e.released},
      {"startNodeId",  e.start_node_id},
      {"endNodeId",    e.end_node_id},
      {"actions",      e.actions},
    };
    if (!e.edge_description.empty())   j["edgeDescription"]   = e.edge_description;
    if (e.max_speed >= 0.0)            j["maxSpeed"]          = e.max_speed;
    if (e.max_height >= 0.0)           j["maxHeight"]         = e.max_height;
    if (e.min_height >= 0.0)           j["minHeight"]         = e.min_height;
    if (e.orientation.has_value())     j["orientation"]       = e.orientation.value();
    if (!e.orientation_type.empty())   j["orientationType"]   = e.orientation_type;
    if (!e.direction.empty())          j["direction"]         = e.direction;
    if (e.rotation_allowed.has_value()) {
      j["rotationAllowed"] = e.rotation_allowed.value();
    }
    if (e.max_rotation_speed >= 0.0)   j["maxRotationSpeed"]  = e.max_rotation_speed;
    if (e.length >= 0.0)               j["length"]            = e.length;
    if (e.trajectory.has_value())      j["trajectory"]        = e.trajectory.value();
    if (e.corridor.has_value())        j["corridor"]          = e.corridor.value();
  }
  static void from_json(const json& j, vda5050::Edge& e) {
    j.at("edgeId").get_to(e.edge_id);
    j.at("sequenceId").get_to(e.sequence_id);
    j.at("released").get_to(e.released);
    j.at("startNodeId").get_to(e.start_node_id);
    j.at("endNodeId").get_to(e.end_node_id);
    j.at("actions").get_to(e.actions);
    if (j.contains("edgeDescription"))  j["edgeDescription"].get_to(e.edge_description);
    if (j.contains("maxSpeed"))         j["maxSpeed"].get_to(e.max_speed);
    if (j.contains("maxHeight"))        j["maxHeight"].get_to(e.max_height);
    if (j.contains("minHeight"))        j["minHeight"].get_to(e.min_height);
    if (j.contains("orientation"))      e.orientation = j["orientation"].get<double>();
    if (j.contains("orientationType"))  j["orientationType"].get_to(e.orientation_type);
    if (j.contains("direction"))        j["direction"].get_to(e.direction);
    if (j.contains("rotationAllowed"))  {
      e.rotation_allowed = j["rotationAllowed"].get<bool>();
    }
    if (j.contains("maxRotationSpeed")) j["maxRotationSpeed"].get_to(e.max_rotation_speed);
    if (j.contains("length"))           j["length"].get_to(e.length);
    if (j.contains("trajectory") && !j["trajectory"].is_null()) {
      e.trajectory = j["trajectory"].get<vda5050::Trajectory>();
    }
    if (j.contains("corridor") && !j["corridor"].is_null()) {
      e.corridor = j["corridor"].get<vda5050::Corridor>();
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Order / InstantActions
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::Order> {
  static void to_json(json& j, const vda5050::Order& o) {
    j = o.header;
    j["orderId"]        = o.order_id;
    j["orderUpdateId"]  = o.order_update_id;
    j["nodes"]          = o.nodes;
    j["edges"]          = o.edges;
    if (!o.zone_set_id.empty()) j["zoneSetId"] = o.zone_set_id;
  }
  static void from_json(const json& j, vda5050::Order& o) {
    o.header = j.get<vda5050::Header>();
    j.at("orderId").get_to(o.order_id);
    j.at("orderUpdateId").get_to(o.order_update_id);
    j.at("nodes").get_to(o.nodes);
    j.at("edges").get_to(o.edges);
    if (j.contains("zoneSetId")) j["zoneSetId"].get_to(o.zone_set_id);
  }
};

template<>
struct adl_serializer<vda5050::InstantActions> {
  static void to_json(json& j, const vda5050::InstantActions& ia) {
    j = ia.header;
    j["actions"] = ia.actions;
  }
  static void from_json(const json& j, vda5050::InstantActions& ia) {
    ia.header = j.get<vda5050::Header>();
    j.at("actions").get_to(ia.actions);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// State sub-types
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::NodeState> {
  static void to_json(json& j, const vda5050::NodeState& ns) {
    j = {
      {"nodeId",     ns.node_id},
      {"sequenceId", ns.sequence_id},
      {"released",   ns.released},
    };
    if (!ns.node_description.empty())  j["nodeDescription"] = ns.node_description;
    if (ns.node_position.has_value())  j["nodePosition"]    = ns.node_position.value();
  }
  static void from_json(const json& j, vda5050::NodeState& ns) {
    j.at("nodeId").get_to(ns.node_id);
    j.at("sequenceId").get_to(ns.sequence_id);
    j.at("released").get_to(ns.released);
    if (j.contains("nodeDescription")) j["nodeDescription"].get_to(ns.node_description);
    if (j.contains("nodePosition") && !j["nodePosition"].is_null()) {
      ns.node_position = j["nodePosition"].get<vda5050::NodePosition>();
    }
  }
};

template<>
struct adl_serializer<vda5050::EdgeState> {
  static void to_json(json& j, const vda5050::EdgeState& es) {
    j = {
      {"edgeId",     es.edge_id},
      {"sequenceId", es.sequence_id},
      {"released",   es.released},
    };
    if (!es.edge_description.empty()) j["edgeDescription"] = es.edge_description;
    if (es.trajectory.has_value())    j["trajectory"]      = es.trajectory.value();
  }
  static void from_json(const json& j, vda5050::EdgeState& es) {
    j.at("edgeId").get_to(es.edge_id);
    j.at("sequenceId").get_to(es.sequence_id);
    j.at("released").get_to(es.released);
    if (j.contains("edgeDescription")) j["edgeDescription"].get_to(es.edge_description);
    if (j.contains("trajectory") && !j["trajectory"].is_null()) {
      es.trajectory = j["trajectory"].get<vda5050::Trajectory>();
    }
  }
};

template<>
struct adl_serializer<vda5050::AgvPosition> {
  static void to_json(json& j, const vda5050::AgvPosition& p) {
    j = {
      {"positionInitialized", p.position_initialized},
      {"x",                   p.x},
      {"y",                   p.y},
      {"theta",               p.theta},
      {"mapId",               p.map_id},
    };
    if (p.localization_score >= 0.0) j["localizationScore"] = p.localization_score;
    if (p.deviation_range >= 0.0)    j["deviationRange"]    = p.deviation_range;
    if (!p.map_description.empty())  j["mapDescription"]    = p.map_description;
  }
  static void from_json(const json& j, vda5050::AgvPosition& p) {
    j.at("positionInitialized").get_to(p.position_initialized);
    j.at("x").get_to(p.x);
    j.at("y").get_to(p.y);
    j.at("theta").get_to(p.theta);
    j.at("mapId").get_to(p.map_id);
    if (j.contains("localizationScore")) j["localizationScore"].get_to(p.localization_score);
    if (j.contains("deviationRange"))    j["deviationRange"].get_to(p.deviation_range);
    if (j.contains("mapDescription"))    j["mapDescription"].get_to(p.map_description);
  }
};

template<>
struct adl_serializer<vda5050::Velocity> {
  static void to_json(json& j, const vda5050::Velocity& v) {
    j = {{"vx", v.vx}, {"vy", v.vy}, {"omega", v.omega}};
  }
  static void from_json(const json& j, vda5050::Velocity& v) {
    if (j.contains("vx"))    j["vx"].get_to(v.vx);
    if (j.contains("vy"))    j["vy"].get_to(v.vy);
    if (j.contains("omega")) j["omega"].get_to(v.omega);
  }
};

template<>
struct adl_serializer<vda5050::BoundingBoxReference> {
  static void to_json(json& j, const vda5050::BoundingBoxReference& b) {
    j = {{"x", b.x}, {"y", b.y}, {"z", b.z}};
    if (b.theta != 0.0) j["theta"] = b.theta;
  }
  static void from_json(const json& j, vda5050::BoundingBoxReference& b) {
    j.at("x").get_to(b.x);
    j.at("y").get_to(b.y);
    j.at("z").get_to(b.z);
    if (j.contains("theta")) j["theta"].get_to(b.theta);
  }
};

template<>
struct adl_serializer<vda5050::LoadDimensions> {
  static void to_json(json& j, const vda5050::LoadDimensions& d) {
    j = {{"length", d.length}, {"width", d.width}};
    if (d.height >= 0.0) j["height"] = d.height;
  }
  static void from_json(const json& j, vda5050::LoadDimensions& d) {
    if (j.contains("length")) j["length"].get_to(d.length);
    if (j.contains("width"))  j["width"].get_to(d.width);
    if (j.contains("height")) j["height"].get_to(d.height);
  }
};

template<>
struct adl_serializer<vda5050::Load> {
  static void to_json(json& j, const vda5050::Load& l) {
    j = json::object();
    if (!l.load_id.empty())       j["loadId"]       = l.load_id;
    if (!l.load_type.empty())     j["loadType"]     = l.load_type;
    if (!l.load_position.empty()) j["loadPosition"] = l.load_position;
    if (l.weight >= 0.0)          j["weight"]       = l.weight;
    if (l.bounding_box_reference.has_value()) j["boundingBoxReference"] = l.bounding_box_reference.value();
    if (l.load_dimensions.has_value())        j["loadDimensions"]       = l.load_dimensions.value();
  }
  static void from_json(const json& j, vda5050::Load& l) {
    if (j.contains("loadId"))       j["loadId"].get_to(l.load_id);
    if (j.contains("loadType"))     j["loadType"].get_to(l.load_type);
    if (j.contains("loadPosition")) j["loadPosition"].get_to(l.load_position);
    if (j.contains("weight"))       j["weight"].get_to(l.weight);
    if (j.contains("boundingBoxReference") && !j["boundingBoxReference"].is_null()) {
      l.bounding_box_reference = j["boundingBoxReference"].get<vda5050::BoundingBoxReference>();
    }
    if (j.contains("loadDimensions") && !j["loadDimensions"].is_null()) {
      l.load_dimensions = j["loadDimensions"].get<vda5050::LoadDimensions>();
    }
  }
};

template<>
struct adl_serializer<vda5050::ActionState> {
  static void to_json(json& j, const vda5050::ActionState& as) {
    j = {
      {"actionId",     as.action_id},
      {"actionType",   as.action_type},
      {"actionStatus", as.action_status},
    };
    if (!as.action_description.empty())  j["actionDescription"]  = as.action_description;
    if (!as.result_description.empty())  j["resultDescription"]  = as.result_description;
  }
  static void from_json(const json& j, vda5050::ActionState& as) {
    j.at("actionId").get_to(as.action_id);
    j.at("actionType").get_to(as.action_type);
    j.at("actionStatus").get_to(as.action_status);
    if (j.contains("actionDescription")) j["actionDescription"].get_to(as.action_description);
    if (j.contains("resultDescription")) j["resultDescription"].get_to(as.result_description);
  }
};

template<>
struct adl_serializer<vda5050::BatteryState> {
  static void to_json(json& j, const vda5050::BatteryState& b) {
    j = {
      {"batteryCharge", b.battery_charge},
      {"charging",      b.charging},
    };
    if (b.battery_voltage >= 0.0) j["batteryVoltage"] = b.battery_voltage;
    if (b.battery_health >= 0)    j["batteryHealth"]  = b.battery_health;
    if (b.reach > 0)              j["reach"]          = b.reach;
  }
  static void from_json(const json& j, vda5050::BatteryState& b) {
    j.at("batteryCharge").get_to(b.battery_charge);
    j.at("charging").get_to(b.charging);
    if (j.contains("batteryVoltage")) j["batteryVoltage"].get_to(b.battery_voltage);
    if (j.contains("batteryHealth"))  j["batteryHealth"].get_to(b.battery_health);
    if (j.contains("reach"))          j["reach"].get_to(b.reach);
  }
};

template<>
struct adl_serializer<vda5050::ErrorReference> {
  static void to_json(json& j, const vda5050::ErrorReference& r) {
    j = {{"referenceKey", r.reference_key}, {"referenceValue", r.reference_value}};
  }
  static void from_json(const json& j, vda5050::ErrorReference& r) {
    j.at("referenceKey").get_to(r.reference_key);
    j.at("referenceValue").get_to(r.reference_value);
  }
};

template<>
struct adl_serializer<vda5050::Error> {
  static void to_json(json& j, const vda5050::Error& e) {
    j = {
      {"errorType",       e.error_type},
      {"errorLevel",      e.error_level},
      {"errorReferences", e.error_references},
    };
    if (!e.error_description.empty()) j["errorDescription"] = e.error_description;
    if (!e.error_hint.empty())        j["errorHint"]        = e.error_hint;
  }
  static void from_json(const json& j, vda5050::Error& e) {
    j.at("errorType").get_to(e.error_type);
    j.at("errorLevel").get_to(e.error_level);
    if (j.contains("errorReferences"))  j["errorReferences"].get_to(e.error_references);
    if (j.contains("errorDescription")) j["errorDescription"].get_to(e.error_description);
    if (j.contains("errorHint"))        j["errorHint"].get_to(e.error_hint);
  }
};

template<>
struct adl_serializer<vda5050::InfoReference> {
  static void to_json(json& j, const vda5050::InfoReference& r) {
    j = {{"referenceKey", r.reference_key}, {"referenceValue", r.reference_value}};
  }
  static void from_json(const json& j, vda5050::InfoReference& r) {
    j.at("referenceKey").get_to(r.reference_key);
    j.at("referenceValue").get_to(r.reference_value);
  }
};

template<>
struct adl_serializer<vda5050::Info> {
  static void to_json(json& j, const vda5050::Info& i) {
    j = {
      {"infoType",       i.info_type},
      {"infoLevel",      i.info_level},
      {"infoReferences", i.info_references},
    };
    if (!i.info_description.empty()) j["infoDescription"] = i.info_description;
  }
  static void from_json(const json& j, vda5050::Info& i) {
    j.at("infoType").get_to(i.info_type);
    j.at("infoLevel").get_to(i.info_level);
    if (j.contains("infoReferences"))  j["infoReferences"].get_to(i.info_references);
    if (j.contains("infoDescription")) j["infoDescription"].get_to(i.info_description);
  }
};

template<>
struct adl_serializer<vda5050::SafetyState> {
  static void to_json(json& j, const vda5050::SafetyState& s) {
    j = {{"eStop", s.e_stop}, {"fieldViolation", s.field_violation}};
  }
  static void from_json(const json& j, vda5050::SafetyState& s) {
    j.at("eStop").get_to(s.e_stop);
    j.at("fieldViolation").get_to(s.field_violation);
  }
};

template<>
struct adl_serializer<vda5050::MapInfo> {
  static void to_json(json& j, const vda5050::MapInfo& m) {
    j = {{"mapId", m.map_id}, {"mapStatus", m.map_status}};
    if (!m.map_version.empty())     j["mapVersion"]     = m.map_version;
    if (!m.map_description.empty()) j["mapDescription"] = m.map_description;
  }
  static void from_json(const json& j, vda5050::MapInfo& m) {
    j.at("mapId").get_to(m.map_id);
    j.at("mapStatus").get_to(m.map_status);
    if (j.contains("mapVersion"))     j["mapVersion"].get_to(m.map_version);
    if (j.contains("mapDescription")) j["mapDescription"].get_to(m.map_description);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::State> {
  static void to_json(json& j, const vda5050::State& s) {
    j = s.header;
    j["orderId"]                 = s.order_id;
    j["orderUpdateId"]           = s.order_update_id;
    j["lastNodeId"]              = s.last_node_id;
    j["lastNodeSequenceId"]      = s.last_node_sequence_id;
    j["nodeStates"]              = s.node_states;
    j["edgeStates"]              = s.edge_states;
    j["driving"]                 = s.driving;
    j["paused"]                  = s.paused;
    j["newBaseRequest"]          = s.new_base_request;
    j["distanceSinceLastNode"]   = s.distance_since_last_node;
    j["actionStates"]            = s.action_states;
    j["batteryState"]            = s.battery_state;
    j["operatingMode"]           = s.operating_mode;
    j["errors"]                  = s.errors;
    j["information"]             = s.information;
    j["safetyState"]             = s.safety_state;
    if (!s.zone_set_id.empty())          j["zoneSetId"]    = s.zone_set_id;
    if (!s.loads.empty())                j["loads"]        = s.loads;
    if (!s.maps.empty())                 j["maps"]         = s.maps;
    if (s.agv_position.has_value())      j["agvPosition"]  = s.agv_position.value();
    if (s.velocity.has_value())          j["velocity"]     = s.velocity.value();
  }
  static void from_json(const json& j, vda5050::State& s) {
    s.header = j.get<vda5050::Header>();
    j.at("orderId").get_to(s.order_id);
    j.at("orderUpdateId").get_to(s.order_update_id);
    j.at("lastNodeId").get_to(s.last_node_id);
    j.at("lastNodeSequenceId").get_to(s.last_node_sequence_id);
    j.at("nodeStates").get_to(s.node_states);
    j.at("edgeStates").get_to(s.edge_states);
    j.at("driving").get_to(s.driving);
    j.at("paused").get_to(s.paused);
    j.at("newBaseRequest").get_to(s.new_base_request);
    j.at("distanceSinceLastNode").get_to(s.distance_since_last_node);
    j.at("actionStates").get_to(s.action_states);
    j.at("batteryState").get_to(s.battery_state);
    j.at("operatingMode").get_to(s.operating_mode);
    j.at("errors").get_to(s.errors);
    j.at("information").get_to(s.information);
    j.at("safetyState").get_to(s.safety_state);
    if (j.contains("zoneSetId")) j["zoneSetId"].get_to(s.zone_set_id);
    if (j.contains("loads"))     j["loads"].get_to(s.loads);
    if (j.contains("maps"))      j["maps"].get_to(s.maps);
    if (j.contains("agvPosition") && !j["agvPosition"].is_null()) {
      s.agv_position = j["agvPosition"].get<vda5050::AgvPosition>();
    }
    if (j.contains("velocity") && !j["velocity"].is_null()) {
      s.velocity = j["velocity"].get<vda5050::Velocity>();
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection / Visualization
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::Connection> {
  static void to_json(json& j, const vda5050::Connection& c) {
    j = c.header;
    j["connectionState"] = c.connection_state;
  }
  static void from_json(const json& j, vda5050::Connection& c) {
    c.header = j.get<vda5050::Header>();
    j.at("connectionState").get_to(c.connection_state);
  }
};

template<>
struct adl_serializer<vda5050::Visualization> {
  static void to_json(json& j, const vda5050::Visualization& v) {
    j = v.header;
    if (v.agv_position.has_value()) j["agvPosition"] = v.agv_position.value();
    if (v.velocity.has_value())     j["velocity"]    = v.velocity.value();
  }
  static void from_json(const json& j, vda5050::Visualization& v) {
    v.header = j.get<vda5050::Header>();
    if (j.contains("agvPosition") && !j["agvPosition"].is_null()) {
      v.agv_position = j["agvPosition"].get<vda5050::AgvPosition>();
    }
    if (j.contains("velocity") && !j["velocity"].is_null()) {
      v.velocity = j["velocity"].get<vda5050::Velocity>();
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Factsheet
// ─────────────────────────────────────────────────────────────────────────────

template<>
struct adl_serializer<vda5050::TypeSpecification> {
  static void to_json(json& j, const vda5050::TypeSpecification& t) {
    j = {
      {"seriesName",          t.series_name},
      {"agvKinematic",        t.agv_kinematic},
      {"agvClass",            t.agv_class},
      {"maxLoadMass",         t.max_load_mass},
      {"localizationTypes",   t.localization_types},
      {"navigationTypes",     t.navigation_types},
    };
    if (!t.series_description.empty()) j["seriesDescription"] = t.series_description;
  }
  static void from_json(const json& j, vda5050::TypeSpecification& t) {
    j.at("seriesName").get_to(t.series_name);
    j.at("agvKinematic").get_to(t.agv_kinematic);
    j.at("agvClass").get_to(t.agv_class);
    j.at("maxLoadMass").get_to(t.max_load_mass);
    j.at("localizationTypes").get_to(t.localization_types);
    j.at("navigationTypes").get_to(t.navigation_types);
    if (j.contains("seriesDescription")) j["seriesDescription"].get_to(t.series_description);
  }
};

template<>
struct adl_serializer<vda5050::PhysicalParameters> {
  static void to_json(json& j, const vda5050::PhysicalParameters& p) {
    j = {
      {"speedMin",         p.speed_min},
      {"speedMax",         p.speed_max},
      {"accelerationMax",  p.acceleration_max},
      {"decelerationMax",  p.deceleration_max},
      {"width",            p.width},
      {"length",           p.length},
    };
    VDA_TO_OPT(j, heightMin, p.height_min)
    VDA_TO_OPT(j, heightMax, p.height_max)
  }
  static void from_json(const json& j, vda5050::PhysicalParameters& p) {
    j.at("speedMin").get_to(p.speed_min);
    j.at("speedMax").get_to(p.speed_max);
    j.at("accelerationMax").get_to(p.acceleration_max);
    j.at("decelerationMax").get_to(p.deceleration_max);
    j.at("width").get_to(p.width);
    j.at("length").get_to(p.length);
    VDA_FROM_OPT(j, heightMin, p.height_min)
    VDA_FROM_OPT(j, heightMax, p.height_max)
  }
};

template<>
struct adl_serializer<vda5050::MaxStringLens> {
  static void to_json(json& j, const vda5050::MaxStringLens& m) {
    j = json::object();
    VDA_TO_OPT(j, msgLen,          m.msg_len)
    VDA_TO_OPT(j, topicSerialLen,  m.topic_serial_len)
    VDA_TO_OPT(j, topicElemLen,    m.topic_elem_len)
    VDA_TO_OPT(j, idLen,           m.id_len)
    VDA_TO_OPT(j, idNumericalOnly, m.id_numerical_only)
    VDA_TO_OPT(j, enumLen,         m.enum_len)
    VDA_TO_OPT(j, loadIdLen,       m.load_id_len)
  }
  static void from_json(const json& j, vda5050::MaxStringLens& m) {
    VDA_FROM_OPT(j, msgLen,          m.msg_len)
    VDA_FROM_OPT(j, topicSerialLen,  m.topic_serial_len)
    VDA_FROM_OPT(j, topicElemLen,    m.topic_elem_len)
    VDA_FROM_OPT(j, idLen,           m.id_len)
    VDA_FROM_OPT(j, idNumericalOnly, m.id_numerical_only)
    VDA_FROM_OPT(j, enumLen,         m.enum_len)
    VDA_FROM_OPT(j, loadIdLen,       m.load_id_len)
  }
};

// NOTE: VDA5050 §9.4 uses dot-notation as literal JSON key names inside maxArrayLens,
// e.g. the key is the string "order.nodes", NOT a nested object.
// The VDA_TO_OPT / VDA_FROM_OPT macros stringify to #key, so we must use the exact
// identifier that matches the desired JSON key — we write it manually here.
template<>
struct adl_serializer<vda5050::MaxArrayLens> {
  static void to_json(json& j, const vda5050::MaxArrayLens& m) {
    j = json::object();
    // Order
    if (m.order_nodes)               j["order.nodes"]                   = *m.order_nodes;
    if (m.order_edges)               j["order.edges"]                   = *m.order_edges;
    if (m.node_actions)              j["node.actions"]                  = *m.node_actions;
    if (m.edge_actions)              j["edge.actions"]                  = *m.edge_actions;
    if (m.actions_action_parameters) j["actions.actionsParameters"]     = *m.actions_action_parameters;
    if (m.instant_actions)           j["instantActions"]                = *m.instant_actions;
    // Trajectory
    if (m.trajectory_knot_vector)    j["trajectory.knotVector"]         = *m.trajectory_knot_vector;
    if (m.trajectory_control_points) j["trajectory.controlPoints"]      = *m.trajectory_control_points;
    // State
    if (m.state_node_states)         j["state.nodeStates"]              = *m.state_node_states;
    if (m.state_edge_states)         j["state.edgeStates"]              = *m.state_edge_states;
    if (m.state_loads)               j["state.loads"]                   = *m.state_loads;
    if (m.state_action_states)       j["state.actionStates"]            = *m.state_action_states;
    if (m.state_errors)              j["state.errors"]                  = *m.state_errors;
    if (m.state_information)         j["state.information"]             = *m.state_information;
    // Error / Info
    if (m.error_error_references)    j["error.errorReferences"]         = *m.error_error_references;
    if (m.info_info_references)      j["information.infoReferences"]    = *m.info_info_references;
  }
  static void from_json(const json& j, vda5050::MaxArrayLens& m) {
    if (j.contains("order.nodes"))               m.order_nodes               = j["order.nodes"].get<uint32_t>();
    if (j.contains("order.edges"))               m.order_edges               = j["order.edges"].get<uint32_t>();
    if (j.contains("node.actions"))              m.node_actions              = j["node.actions"].get<uint32_t>();
    if (j.contains("edge.actions"))              m.edge_actions              = j["edge.actions"].get<uint32_t>();
    if (j.contains("actions.actionsParameters")) m.actions_action_parameters = j["actions.actionsParameters"].get<uint32_t>();
    if (j.contains("instantActions"))            m.instant_actions           = j["instantActions"].get<uint32_t>();
    if (j.contains("trajectory.knotVector"))     m.trajectory_knot_vector    = j["trajectory.knotVector"].get<uint32_t>();
    if (j.contains("trajectory.controlPoints"))  m.trajectory_control_points = j["trajectory.controlPoints"].get<uint32_t>();
    if (j.contains("state.nodeStates"))          m.state_node_states         = j["state.nodeStates"].get<uint32_t>();
    if (j.contains("state.edgeStates"))          m.state_edge_states         = j["state.edgeStates"].get<uint32_t>();
    if (j.contains("state.loads"))               m.state_loads               = j["state.loads"].get<uint32_t>();
    if (j.contains("state.actionStates"))        m.state_action_states       = j["state.actionStates"].get<uint32_t>();
    if (j.contains("state.errors"))              m.state_errors              = j["state.errors"].get<uint32_t>();
    if (j.contains("state.information"))         m.state_information         = j["state.information"].get<uint32_t>();
    if (j.contains("error.errorReferences"))     m.error_error_references    = j["error.errorReferences"].get<uint32_t>();
    if (j.contains("information.infoReferences")) m.info_info_references     = j["information.infoReferences"].get<uint32_t>();
  }
};

template<>
struct adl_serializer<vda5050::TimingLimits> {
  static void to_json(json& j, const vda5050::TimingLimits& t) {
    j = json::object();
    VDA_TO_OPT(j, minOrderInterval,       t.min_order_interval)
    VDA_TO_OPT(j, minStateInterval,       t.min_state_interval)
    VDA_TO_OPT(j, defaultStateInterval,   t.default_state_interval)
    VDA_TO_OPT(j, visualizationInterval,  t.visualization_interval)
  }
  static void from_json(const json& j, vda5050::TimingLimits& t) {
    VDA_FROM_OPT(j, minOrderInterval,      t.min_order_interval)
    VDA_FROM_OPT(j, minStateInterval,      t.min_state_interval)
    VDA_FROM_OPT(j, defaultStateInterval,  t.default_state_interval)
    VDA_FROM_OPT(j, visualizationInterval, t.visualization_interval)
  }
};

template<>
struct adl_serializer<vda5050::ProtocolLimits> {
  static void to_json(json& j, const vda5050::ProtocolLimits& p) {
    j = {
      {"maxStringLens", p.max_string_lens},
      {"maxArrayLens",  p.max_array_lens},
      {"timing",        p.timing},
    };
  }
  static void from_json(const json& j, vda5050::ProtocolLimits& p) {
    if (j.contains("maxStringLens")) j["maxStringLens"].get_to(p.max_string_lens);
    if (j.contains("maxArrayLens"))  j["maxArrayLens"].get_to(p.max_array_lens);
    if (j.contains("timing"))        j["timing"].get_to(p.timing);
  }
};

template<>
struct adl_serializer<vda5050::OptionalParameter> {
  static void to_json(json& j, const vda5050::OptionalParameter& o) {
    j = {{"parameter", o.parameter}, {"support", o.support}};
    if (!o.description.empty()) j["description"] = o.description;
  }
  static void from_json(const json& j, vda5050::OptionalParameter& o) {
    j.at("parameter").get_to(o.parameter);
    j.at("support").get_to(o.support);
    if (j.contains("description")) j["description"].get_to(o.description);
  }
};

template<>
struct adl_serializer<vda5050::AgvActionParameter> {
  static void to_json(json& j, const vda5050::AgvActionParameter& p) {
    j = {
      {"key",           p.key},
      {"valueDataType", p.value_data_type},
      {"description",   p.description},
      {"isOptional",    p.is_optional},
    };
  }
  static void from_json(const json& j, vda5050::AgvActionParameter& p) {
    j.at("key").get_to(p.key);
    j.at("valueDataType").get_to(p.value_data_type);
    j.at("description").get_to(p.description);
    if (j.contains("isOptional")) j["isOptional"].get_to(p.is_optional);
  }
};

template<>
struct adl_serializer<vda5050::AgvAction> {
  static void to_json(json& j, const vda5050::AgvAction& a) {
    j = {{"actionType", a.action_type}, {"actionScopes", a.action_scopes}};
    if (!a.action_description.empty())  j["actionDescription"]  = a.action_description;
    if (!a.action_parameters.empty())   j["actionParameters"]   = a.action_parameters;
    if (!a.result_description.empty())  j["resultDescription"]  = a.result_description;
    if (!a.blocking_types.empty())      j["blockingTypes"]      = a.blocking_types;
  }
  static void from_json(const json& j, vda5050::AgvAction& a) {
    j.at("actionType").get_to(a.action_type);
    j.at("actionScopes").get_to(a.action_scopes);
    if (j.contains("actionDescription")) j["actionDescription"].get_to(a.action_description);
    if (j.contains("actionParameters"))  j["actionParameters"].get_to(a.action_parameters);
    if (j.contains("resultDescription")) j["resultDescription"].get_to(a.result_description);
    if (j.contains("blockingTypes"))     j["blockingTypes"].get_to(a.blocking_types);
  }
};

template<>
struct adl_serializer<vda5050::ProtocolFeatures> {
  static void to_json(json& j, const vda5050::ProtocolFeatures& f) {
    j = {
      {"optionalParameters", f.optional_parameters},
      {"agvActions",         f.agv_actions},
    };
  }
  static void from_json(const json& j, vda5050::ProtocolFeatures& f) {
    if (j.contains("optionalParameters")) j["optionalParameters"].get_to(f.optional_parameters);
    if (j.contains("agvActions"))         j["agvActions"].get_to(f.agv_actions);
  }
};

template<>
struct adl_serializer<vda5050::Factsheet> {
  static void to_json(json& j, const vda5050::Factsheet& f) {
    j = f.header;
    j["typeSpecification"]  = f.type_specification;
    j["physicalParameters"] = f.physical_parameters;
    j["protocolLimits"]     = f.protocol_limits;
    j["protocolFeatures"]   = f.protocol_features;
  }
  static void from_json(const json& j, vda5050::Factsheet& f) {
    f.header = j.get<vda5050::Header>();
    j.at("typeSpecification").get_to(f.type_specification);
    j.at("physicalParameters").get_to(f.physical_parameters);
    if (j.contains("protocolLimits"))   j["protocolLimits"].get_to(f.protocol_limits);
    if (j.contains("protocolFeatures")) j["protocolFeatures"].get_to(f.protocol_features);
  }
};

}  // namespace nlohmann

#undef VDA_FROM_OPT
#undef VDA_TO_OPT
