/**
 * @file test_converters.cpp
 * @brief Unit tests for json_converter.hpp and ros_converters.hpp.
 *
 * Coverage:
 *  - JSON round-trip for all top-level message types
 *  - Optional fields omitted when not set / preserved when set
 *  - Enum string values match VDA5050 spec (camelCase keys)
 *  - maxArrayLens uses dot-notation key names (VDA5050 §9.4)
 *  - agvActions contains resultDescription and blockingTypes
 *  - ROS ↔ internal converters for all structs
 *  - std::optional<T> adl_serializer generic handler
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "vda5050_client_adapter/json_converter.hpp"
#include "vda5050_client_adapter/ros_converters.hpp"

using json = nlohmann::json;

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────────

vda5050::Header make_header(uint32_t id = 1) {
  vda5050::Header h;
  h.header_id    = id;
  h.timestamp    = "2024-01-01T00:00:00.000Z";
  h.version      = "2.1.0";
  h.manufacturer = "ACME";
  h.serial_number = "0001";
  return h;
}

vda5050::Action make_action(const std::string& id, vda5050::BlockingType bt =
                              vda5050::BlockingType::NONE) {
  vda5050::Action a;
  a.action_id     = id;
  a.action_type   = "noop";
  a.blocking_type = bt;
  return a;
}

vda5050::Node make_node_base(const std::string& id, uint32_t seq, bool released) {
  vda5050::Node n;
  n.node_id     = id;
  n.sequence_id = seq;
  n.released    = released;
  n.actions     = {make_action("act-" + id)};
  return n;
}

vda5050::Edge make_edge_base(const std::string& id, uint32_t seq) {
  vda5050::Edge e;
  e.edge_id       = id;
  e.sequence_id   = seq;
  e.released      = true;
  e.start_node_id = "n1";
  e.end_node_id   = "n2";
  return e;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Header JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, HeaderRoundTrip) {
  const auto h = make_header(42);
  const auto rt = json(h).get<vda5050::Header>();

  EXPECT_EQ(rt.header_id,    42u);
  EXPECT_EQ(rt.timestamp,    h.timestamp);
  EXPECT_EQ(rt.version,      h.version);
  EXPECT_EQ(rt.manufacturer, h.manufacturer);
  EXPECT_EQ(rt.serial_number, h.serial_number);
}

TEST(JsonConverterTest, HeaderUsesCorrectCamelCaseKeys) {
  const json j = make_header(1);
  EXPECT_TRUE(j.contains("headerId"));
  EXPECT_TRUE(j.contains("timestamp"));
  EXPECT_TRUE(j.contains("version"));
  EXPECT_TRUE(j.contains("manufacturer"));
  EXPECT_TRUE(j.contains("serialNumber"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Action JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, ActionRoundTrip) {
  vda5050::Action a;
  a.action_id          = "a1";
  a.action_type        = "pick";
  a.action_description = "Pick item from shelf";
  a.blocking_type      = vda5050::BlockingType::HARD;
  a.action_parameters  = {{"targetId", "shelf-3"}, {"layer", "2"}};

  const auto rt = json(a).get<vda5050::Action>();

  EXPECT_EQ(rt.action_id,          a.action_id);
  EXPECT_EQ(rt.action_type,        a.action_type);
  EXPECT_EQ(rt.action_description, a.action_description);
  EXPECT_EQ(rt.blocking_type,      vda5050::BlockingType::HARD);
  ASSERT_EQ(rt.action_parameters.size(), 2u);
  EXPECT_EQ(rt.action_parameters[0].key,   "targetId");
  EXPECT_EQ(rt.action_parameters[0].value, "shelf-3");
}

TEST(JsonConverterTest, ActionJsonUsesCorrectFieldNames) {
  const json j = make_action("a1", vda5050::BlockingType::SOFT);
  EXPECT_TRUE(j.contains("actionType"));
  EXPECT_TRUE(j.contains("actionId"));
  EXPECT_TRUE(j.contains("blockingType"));
  EXPECT_EQ(j.at("blockingType").get<std::string>(), "SOFT");
}

TEST(JsonConverterTest, ActionDescriptionOmittedWhenEmpty) {
  const json j = make_action("a1");
  EXPECT_FALSE(j.contains("actionDescription"));
}

// ─────────────────────────────────────────────────────────────────────────────
// NodePosition JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, NodePositionRoundTrip) {
  vda5050::NodePosition p;
  p.x             = 1.5;
  p.y             = 2.5;
  p.map_id        = "map1";
  p.theta         = 1.57;
  p.theta_set     = true;
  p.allowed_deviation_xy    = 0.1;
  p.allowed_deviation_theta = 0.05;
  p.map_description = "Floor 1";

  const auto rt = json(p).get<vda5050::NodePosition>();

  EXPECT_DOUBLE_EQ(rt.x, 1.5);
  EXPECT_DOUBLE_EQ(rt.y, 2.5);
  EXPECT_EQ(rt.map_id, "map1");
  EXPECT_TRUE(rt.theta_set);
  EXPECT_DOUBLE_EQ(rt.theta, 1.57);
  EXPECT_DOUBLE_EQ(rt.allowed_deviation_xy, 0.1);
  EXPECT_EQ(rt.map_description, "Floor 1");
}

TEST(JsonConverterTest, NodePositionOmitsThetaWhenNotSet) {
  vda5050::NodePosition p;
  p.x = 0.0; p.y = 0.0; p.map_id = "m";
  p.theta_set = false;
  const json j = p;
  EXPECT_FALSE(j.contains("theta"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge JSON — optional fields
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, EdgeOmitsUnsetOptionalFields) {
  const json j = make_edge_base("e1", 1);
  EXPECT_FALSE(j.contains("orientation"));
  EXPECT_FALSE(j.contains("rotationAllowed"));
  EXPECT_FALSE(j.contains("trajectory"));
  EXPECT_FALSE(j.contains("corridor"));
}

TEST(JsonConverterTest, EdgeRoundTripPreservesOptionalFields) {
  auto e = make_edge_base("e1", 1);
  e.orientation     = 0.785;
  e.rotation_allowed = true;
  e.max_speed       = 1.2;

  vda5050::Trajectory traj;
  traj.degree = 1.0;
  traj.knot_vector   = {0.0, 1.0};
  traj.control_points = {{1.0, 2.0, 1.0}};
  e.trajectory = traj;

  const auto rt = json(e).get<vda5050::Edge>();

  ASSERT_TRUE(rt.orientation.has_value());
  EXPECT_DOUBLE_EQ(*rt.orientation, 0.785);
  ASSERT_TRUE(rt.rotation_allowed.has_value());
  EXPECT_TRUE(*rt.rotation_allowed);
  EXPECT_DOUBLE_EQ(rt.max_speed, 1.2);
  ASSERT_TRUE(rt.trajectory.has_value());
  ASSERT_EQ(rt.trajectory->control_points.size(), 1u);
}

TEST(JsonConverterTest, CorridorRoundTrip) {
  auto e = make_edge_base("e1", 1);
  vda5050::Corridor c;
  c.left_width  = 0.5;
  c.right_width = 0.3;
  c.corridor_ref_point = "KINEMATICCENTER";
  e.corridor = c;

  const auto rt = json(e).get<vda5050::Edge>();
  ASSERT_TRUE(rt.corridor.has_value());
  EXPECT_DOUBLE_EQ(rt.corridor->left_width, 0.5);
  EXPECT_EQ(rt.corridor->corridor_ref_point, "KINEMATICCENTER");
}

// ─────────────────────────────────────────────────────────────────────────────
// Order JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, OrderRoundTrip) {
  vda5050::Order o;
  o.header          = make_header();
  o.order_id        = "ord-1";
  o.order_update_id = 3;
  o.zone_set_id     = "zone-A";
  o.nodes.push_back(make_node_base("n1", 0, true));
  o.nodes.push_back(make_node_base("n2", 2, false));
  o.edges.push_back(make_edge_base("e12", 1));

  const auto rt = json(o).get<vda5050::Order>();

  EXPECT_EQ(rt.order_id,        o.order_id);
  EXPECT_EQ(rt.order_update_id, 3u);
  EXPECT_EQ(rt.zone_set_id,     "zone-A");
  ASSERT_EQ(rt.nodes.size(), 2u);
  ASSERT_EQ(rt.edges.size(), 1u);
  EXPECT_EQ(rt.nodes[0].node_id, "n1");
  EXPECT_TRUE(rt.nodes[0].released);
  EXPECT_FALSE(rt.nodes[1].released);
}

TEST(JsonConverterTest, OrderJsonKeysMatchSpec) {
  vda5050::Order o;
  o.header = make_header();
  o.order_id = "x"; o.order_update_id = 0;
  o.nodes.push_back(make_node_base("n1", 0, true));
  const json j = o;

  EXPECT_TRUE(j.contains("headerId"));
  EXPECT_TRUE(j.contains("orderId"));
  EXPECT_TRUE(j.contains("orderUpdateId"));
  EXPECT_TRUE(j.contains("nodes"));
  EXPECT_TRUE(j.contains("edges"));
  EXPECT_FALSE(j.contains("zoneSetId"));  // not set, must be omitted
}

// ─────────────────────────────────────────────────────────────────────────────
// InstantActions JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, InstantActionsRoundTrip) {
  vda5050::InstantActions ia;
  ia.header  = make_header();
  ia.actions = {make_action("ia1"), make_action("ia2")};

  const auto rt = json(ia).get<vda5050::InstantActions>();

  ASSERT_EQ(rt.actions.size(), 2u);
  EXPECT_EQ(rt.actions[0].action_id, "ia1");
  EXPECT_EQ(rt.actions[1].action_id, "ia2");
}

// ─────────────────────────────────────────────────────────────────────────────
// State JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, StateRoundTrip) {
  vda5050::State s;
  s.header                 = make_header();
  s.order_id               = "o1";
  s.order_update_id        = 1;
  s.last_node_id           = "n1";
  s.last_node_sequence_id  = 0;
  s.driving                = true;
  s.paused                 = false;
  s.new_base_request       = false;
  s.distance_since_last_node = 2.5;
  s.battery_state.battery_charge = 80.0;
  s.battery_state.charging = false;
  s.safety_state.e_stop    = vda5050::EStop::NONE;
  s.operating_mode         = vda5050::OperatingMode::AUTOMATIC;

  const auto rt = json(s).get<vda5050::State>();

  EXPECT_EQ(rt.order_id, "o1");
  EXPECT_TRUE(rt.driving);
  EXPECT_DOUBLE_EQ(rt.distance_since_last_node, 2.5);
  EXPECT_DOUBLE_EQ(rt.battery_state.battery_charge, 80.0);
  EXPECT_EQ(rt.operating_mode, vda5050::OperatingMode::AUTOMATIC);
}

TEST(JsonConverterTest, StateJsonKeysMatchSpec) {
  vda5050::State s;
  s.header = make_header();
  s.battery_state.battery_charge = 50.0;
  const json j = s;

  EXPECT_TRUE(j.contains("orderId"));
  EXPECT_TRUE(j.contains("orderUpdateId"));
  EXPECT_TRUE(j.contains("lastNodeId"));
  EXPECT_TRUE(j.contains("lastNodeSequenceId"));
  EXPECT_TRUE(j.contains("nodeStates"));
  EXPECT_TRUE(j.contains("edgeStates"));
  EXPECT_TRUE(j.contains("driving"));
  EXPECT_TRUE(j.contains("paused"));
  EXPECT_TRUE(j.contains("newBaseRequest"));
  EXPECT_TRUE(j.contains("distanceSinceLastNode"));
  EXPECT_TRUE(j.contains("actionStates"));
  EXPECT_TRUE(j.contains("batteryState"));
  EXPECT_TRUE(j.contains("operatingMode"));
  EXPECT_TRUE(j.contains("errors"));
  EXPECT_TRUE(j.contains("information"));
  EXPECT_TRUE(j.contains("safetyState"));
}

TEST(JsonConverterTest, StateAgvPositionOmittedWhenNotSet) {
  vda5050::State s;
  s.header = make_header();
  s.battery_state.battery_charge = 0.0;
  const json j = s;
  EXPECT_FALSE(j.contains("agvPosition"));
  EXPECT_FALSE(j.contains("velocity"));
}

TEST(JsonConverterTest, StateAgvPositionIncludedWhenSet) {
  vda5050::State s;
  s.header = make_header();
  s.battery_state.battery_charge = 0.0;
  vda5050::AgvPosition pos;
  pos.x = 1.0; pos.y = 2.0; pos.theta = 0.5;
  pos.map_id = "m"; pos.position_initialized = true;
  s.agv_position = pos;

  const json j = s;
  ASSERT_TRUE(j.contains("agvPosition"));
  EXPECT_DOUBLE_EQ(j["agvPosition"]["x"].get<double>(), 1.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, ConnectionRoundTrip) {
  vda5050::Connection c;
  c.header           = make_header();
  c.connection_state = vda5050::ConnectionState::ONLINE;

  const auto rt = json(c).get<vda5050::Connection>();
  EXPECT_EQ(rt.connection_state, vda5050::ConnectionState::ONLINE);
}

TEST(JsonConverterTest, ConnectionStateEnumValuesMatchSpec) {
  auto check = [](vda5050::ConnectionState state, const std::string& expected) {
    vda5050::Connection c;
    c.header = {};
    c.connection_state = state;
    const json j = c;
    EXPECT_EQ(j.at("connectionState").get<std::string>(), expected);
  };

  check(vda5050::ConnectionState::ONLINE,           "ONLINE");
  check(vda5050::ConnectionState::OFFLINE,          "OFFLINE");
  check(vda5050::ConnectionState::CONNECTIONBROKEN, "CONNECTIONBROKEN");
}

// ─────────────────────────────────────────────────────────────────────────────
// Visualization JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, VisualizationWithPositionAndVelocity) {
  vda5050::Visualization v;
  v.header = make_header();
  vda5050::AgvPosition p;
  p.x = 3.0; p.y = 4.0; p.theta = 0.0; p.map_id = "m"; p.position_initialized = true;
  v.agv_position = p;
  vda5050::Velocity vel;
  vel.vx = 0.5; vel.vy = 0.0; vel.omega = 0.1;
  v.velocity = vel;

  const auto rt = json(v).get<vda5050::Visualization>();
  ASSERT_TRUE(rt.agv_position.has_value());
  EXPECT_DOUBLE_EQ(rt.agv_position->x, 3.0);
  ASSERT_TRUE(rt.velocity.has_value());
  EXPECT_DOUBLE_EQ(rt.velocity->vx, 0.5);
}

TEST(JsonConverterTest, VisualizationOmitsPositionAndVelocityWhenNotSet) {
  vda5050::Visualization v;
  v.header = make_header();
  const json j = v;
  EXPECT_FALSE(j.contains("agvPosition"));
  EXPECT_FALSE(j.contains("velocity"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Factsheet JSON — spec compliance
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, FactsheetMaxArrayLensUsesDotNotationKeys) {
  vda5050::Factsheet fs;
  fs.header = make_header();
  fs.type_specification.series_name   = "S";
  fs.type_specification.agv_kinematic = "DIFF";
  fs.type_specification.agv_class     = "CARRIER";
  fs.physical_parameters.speed_max = 1.0;
  fs.physical_parameters.acceleration_max = 1.0;
  fs.physical_parameters.deceleration_max = 2.0;
  fs.physical_parameters.width  = 0.5;
  fs.physical_parameters.length = 0.8;

  // Set some maxArrayLens values
  fs.protocol_limits.max_array_lens.order_nodes    = 50u;
  fs.protocol_limits.max_array_lens.state_errors   = 10u;
  fs.protocol_limits.max_array_lens.instant_actions = 5u;

  const json j = fs;
  const auto& mal = j.at("protocolLimits").at("maxArrayLens");

  // Keys MUST use dot-notation per VDA5050 §9.4
  EXPECT_TRUE(mal.contains("order.nodes"));
  EXPECT_TRUE(mal.contains("state.errors"));
  EXPECT_TRUE(mal.contains("instantActions"));

  // Must NOT use camelCase
  EXPECT_FALSE(mal.contains("orderNodes"));
  EXPECT_FALSE(mal.contains("stateErrors"));

  EXPECT_EQ(mal.at("order.nodes").get<uint32_t>(), 50u);
  EXPECT_EQ(mal.at("state.errors").get<uint32_t>(), 10u);
}

TEST(JsonConverterTest, FactsheetMaxArrayLensRoundTrip) {
  vda5050::Factsheet fs;
  fs.header = make_header();
  fs.type_specification.series_name = "S";
  fs.type_specification.agv_kinematic = "OMNI";
  fs.type_specification.agv_class = "TUGGER";
  fs.physical_parameters.speed_max = 2.0;
  fs.physical_parameters.acceleration_max = 1.0;
  fs.physical_parameters.deceleration_max = 2.0;
  fs.physical_parameters.width  = 0.6;
  fs.physical_parameters.length = 1.0;

  fs.protocol_limits.max_array_lens.order_nodes              = 100u;
  fs.protocol_limits.max_array_lens.order_edges              = 99u;
  fs.protocol_limits.max_array_lens.node_actions             = 10u;
  fs.protocol_limits.max_array_lens.state_node_states        = 50u;
  fs.protocol_limits.max_array_lens.error_error_references   = 8u;

  const auto rt = json(fs).get<vda5050::Factsheet>();

  ASSERT_TRUE(rt.protocol_limits.max_array_lens.order_nodes.has_value());
  EXPECT_EQ(*rt.protocol_limits.max_array_lens.order_nodes, 100u);
  ASSERT_TRUE(rt.protocol_limits.max_array_lens.state_node_states.has_value());
  EXPECT_EQ(*rt.protocol_limits.max_array_lens.state_node_states, 50u);
  ASSERT_TRUE(rt.protocol_limits.max_array_lens.error_error_references.has_value());
  EXPECT_EQ(*rt.protocol_limits.max_array_lens.error_error_references, 8u);
}

TEST(JsonConverterTest, FactsheetAgvActionHasBlockingTypesAndResultDescription) {
  vda5050::Factsheet fs;
  fs.header = make_header();
  fs.type_specification.series_name = "S";
  fs.type_specification.agv_kinematic = "DIFF";
  fs.type_specification.agv_class = "CARRIER";
  fs.physical_parameters.speed_max = 1.0;
  fs.physical_parameters.acceleration_max = 1.0;
  fs.physical_parameters.deceleration_max = 2.0;
  fs.physical_parameters.width  = 0.5;
  fs.physical_parameters.length = 0.8;

  vda5050::AgvAction action;
  action.action_type        = "pick";
  action.action_scopes      = {"NODE"};
  action.action_description = "Pick a load";
  action.result_description = "Load picked successfully";
  action.blocking_types     = {"HARD"};
  fs.protocol_features.agv_actions.push_back(action);

  const json j = fs;
  const auto& acts = j.at("protocolFeatures").at("agvActions");
  ASSERT_EQ(acts.size(), 1u);

  const auto& act = acts[0];
  EXPECT_EQ(act.at("actionType").get<std::string>(), "pick");
  ASSERT_TRUE(act.contains("resultDescription"));
  EXPECT_EQ(act.at("resultDescription").get<std::string>(), "Load picked successfully");
  ASSERT_TRUE(act.contains("blockingTypes"));
  ASSERT_EQ(act.at("blockingTypes").size(), 1u);
  EXPECT_EQ(act.at("blockingTypes")[0].get<std::string>(), "HARD");
}

TEST(JsonConverterTest, FactsheetTimingLimitsRoundTrip) {
  vda5050::Factsheet fs;
  fs.header = make_header();
  fs.type_specification.series_name = "S";
  fs.type_specification.agv_kinematic = "DIFF";
  fs.type_specification.agv_class = "CARRIER";
  fs.physical_parameters.speed_max = 1.0;
  fs.physical_parameters.acceleration_max = 1.0;
  fs.physical_parameters.deceleration_max = 2.0;
  fs.physical_parameters.width  = 0.5;
  fs.physical_parameters.length = 0.8;

  fs.protocol_limits.timing.default_state_interval  = 30.0;
  fs.protocol_limits.timing.visualization_interval  = 1.0;
  fs.protocol_limits.timing.min_order_interval      = 0.1;

  const auto rt = json(fs).get<vda5050::Factsheet>();

  ASSERT_TRUE(rt.protocol_limits.timing.default_state_interval.has_value());
  EXPECT_DOUBLE_EQ(*rt.protocol_limits.timing.default_state_interval, 30.0);
  ASSERT_TRUE(rt.protocol_limits.timing.visualization_interval.has_value());
  EXPECT_DOUBLE_EQ(*rt.protocol_limits.timing.visualization_interval, 1.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Enum string values
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, BlockingTypeEnumValuesMatchSpec) {
  EXPECT_EQ(json(vda5050::BlockingType::NONE).get<std::string>(), "NONE");
  EXPECT_EQ(json(vda5050::BlockingType::SOFT).get<std::string>(), "SOFT");
  EXPECT_EQ(json(vda5050::BlockingType::HARD).get<std::string>(), "HARD");
}

TEST(JsonConverterTest, ActionStatusEnumValuesMatchSpec) {
  EXPECT_EQ(json(vda5050::ActionStatus::WAITING).get<std::string>(),      "WAITING");
  EXPECT_EQ(json(vda5050::ActionStatus::INITIALIZING).get<std::string>(), "INITIALIZING");
  EXPECT_EQ(json(vda5050::ActionStatus::RUNNING).get<std::string>(),      "RUNNING");
  EXPECT_EQ(json(vda5050::ActionStatus::PAUSED).get<std::string>(),       "PAUSED");
  EXPECT_EQ(json(vda5050::ActionStatus::FINISHED).get<std::string>(),     "FINISHED");
  EXPECT_EQ(json(vda5050::ActionStatus::FAILED).get<std::string>(),       "FAILED");
}

TEST(JsonConverterTest, OperatingModeEnumValuesMatchSpec) {
  EXPECT_EQ(json(vda5050::OperatingMode::AUTOMATIC).get<std::string>(),    "AUTOMATIC");
  EXPECT_EQ(json(vda5050::OperatingMode::SEMIAUTOMATIC).get<std::string>(),"SEMIAUTOMATIC");
  EXPECT_EQ(json(vda5050::OperatingMode::MANUAL).get<std::string>(),       "MANUAL");
  EXPECT_EQ(json(vda5050::OperatingMode::SERVICE).get<std::string>(),      "SERVICE");
  EXPECT_EQ(json(vda5050::OperatingMode::TEACHIN).get<std::string>(),      "TEACHIN");
}

TEST(JsonConverterTest, EStopEnumValuesMatchSpec) {
  EXPECT_EQ(json(vda5050::EStop::AUTOACK).get<std::string>(), "AUTOACK");
  EXPECT_EQ(json(vda5050::EStop::MANUAL).get<std::string>(),  "MANUAL");
  EXPECT_EQ(json(vda5050::EStop::REMOTE).get<std::string>(),  "REMOTE");
  EXPECT_EQ(json(vda5050::EStop::NONE).get<std::string>(),    "NONE");
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic std::optional<T> serializer
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonConverterTest, OptionalSerializerWritesNullWhenUnset) {
  std::optional<double> unset;
  const json j = unset;
  EXPECT_TRUE(j.is_null());
}

TEST(JsonConverterTest, OptionalSerializerWritesValueWhenSet) {
  std::optional<double> set = 3.14;
  const json j = set;
  EXPECT_DOUBLE_EQ(j.get<double>(), 3.14);
}

TEST(JsonConverterTest, OptionalSerializerDeserializesNullToNullopt) {
  const json j = nullptr;
  const auto opt = j.get<std::optional<uint32_t>>();
  EXPECT_FALSE(opt.has_value());
}

TEST(JsonConverterTest, OptionalSerializerDeserializesValueToOptional) {
  const json j = 42;
  const auto opt = j.get<std::optional<uint32_t>>();
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(*opt, 42u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ROS ↔ internal converters
// ─────────────────────────────────────────────────────────────────────────────

TEST(RosConverterTest, HeaderRoundTrip) {
  const auto h = make_header(7);
  const auto rt = vda5050_adapter::internal_from_ros(
                    vda5050_adapter::ros_from_internal(h));

  EXPECT_EQ(rt.header_id,     h.header_id);
  EXPECT_EQ(rt.timestamp,     h.timestamp);
  EXPECT_EQ(rt.version,       h.version);
  EXPECT_EQ(rt.manufacturer,  h.manufacturer);
  EXPECT_EQ(rt.serial_number, h.serial_number);
}

TEST(RosConverterTest, ActionRoundTrip) {
  vda5050::Action a;
  a.action_id          = "a1";
  a.action_type        = "pick";
  a.action_description = "desc";
  a.blocking_type      = vda5050::BlockingType::SOFT;
  a.action_parameters  = {{"k1", "v1"}};

  const auto rt = vda5050_adapter::internal_from_ros(
                    vda5050_adapter::ros_from_internal(a));

  EXPECT_EQ(rt.action_id,          a.action_id);
  EXPECT_EQ(rt.action_type,        a.action_type);
  EXPECT_EQ(rt.action_description, a.action_description);
  EXPECT_EQ(rt.blocking_type,      vda5050::BlockingType::SOFT);
  ASSERT_EQ(rt.action_parameters.size(), 1u);
  EXPECT_EQ(rt.action_parameters[0].key,   "k1");
  EXPECT_EQ(rt.action_parameters[0].value, "v1");
}

TEST(RosConverterTest, NodePositionRoundTrip) {
  vda5050::NodePosition p;
  p.x = 1.0; p.y = 2.0; p.map_id = "m";
  p.theta = 0.5; p.theta_set = true;
  p.allowed_deviation_xy = 0.1;
  p.map_description = "Floor 1";

  const auto rt = vda5050_adapter::internal_from_ros(
                    vda5050_adapter::ros_from_internal(p));

  EXPECT_DOUBLE_EQ(rt.x, 1.0);
  EXPECT_DOUBLE_EQ(rt.y, 2.0);
  EXPECT_EQ(rt.map_id, "m");
  EXPECT_TRUE(rt.theta_set);
  EXPECT_DOUBLE_EQ(rt.theta, 0.5);
  EXPECT_EQ(rt.map_description, "Floor 1");
}

TEST(RosConverterTest, EdgeOptionalFieldsUseNaNAndFlagPattern) {
  auto e = make_edge_base("e1", 1);

  const auto ros_unset = vda5050_adapter::ros_from_internal(e);
  EXPECT_TRUE(std::isnan(ros_unset.orientation));
  EXPECT_FALSE(ros_unset.rotation_allowed_set);
  EXPECT_FALSE(ros_unset.trajectory_set);
  EXPECT_FALSE(ros_unset.corridor_set);

  e.orientation      = 0.75;
  e.rotation_allowed = false;
  const auto ros_set = vda5050_adapter::ros_from_internal(e);
  EXPECT_DOUBLE_EQ(ros_set.orientation, 0.75);
  EXPECT_TRUE(ros_set.rotation_allowed_set);
  EXPECT_FALSE(ros_set.rotation_allowed);
}

TEST(RosConverterTest, AgvPositionRoundTrip) {
  vda5050::AgvPosition p;
  p.position_initialized = true;
  p.x = 1.5; p.y = -0.5; p.theta = 1.57;
  p.map_id = "floor2";
  p.localization_score = 0.98;
  p.deviation_range    = 0.05;

  const auto rt = vda5050_adapter::internal_from_ros(
                    vda5050_adapter::ros_from_internal(p));

  EXPECT_TRUE(rt.position_initialized);
  EXPECT_DOUBLE_EQ(rt.x, 1.5);
  EXPECT_DOUBLE_EQ(rt.y, -0.5);
  EXPECT_EQ(rt.map_id, "floor2");
  EXPECT_DOUBLE_EQ(rt.localization_score, 0.98);
}

TEST(RosConverterTest, BatteryStateRoundTrip) {
  vda5050::BatteryState b;
  b.battery_charge  = 75.0;
  b.battery_voltage = 24.5;
  b.battery_health  = 90;
  b.charging        = false;
  b.reach           = 5000;

  const auto rt = vda5050_adapter::internal_from_ros(
                    vda5050_adapter::ros_from_internal(b));

  EXPECT_DOUBLE_EQ(rt.battery_charge,  75.0);
  EXPECT_DOUBLE_EQ(rt.battery_voltage, 24.5);
  EXPECT_EQ(rt.battery_health, 90);
  EXPECT_FALSE(rt.charging);
  EXPECT_EQ(rt.reach, 5000u);
}

TEST(RosConverterTest, SafetyStateEStopEnumMappings) {
  auto check_estop = [](const std::string& estop_str,
                         vda5050::EStop expected) {
    vda5050_msgs::msg::SafetyState ros;
    ros.e_stop = estop_str;
    ros.field_violation = false;
    const auto internal = vda5050_adapter::internal_from_ros(ros);
    EXPECT_EQ(internal.e_stop, expected) << "For eStop string: " << estop_str;
  };

  check_estop("AUTOACK", vda5050::EStop::AUTOACK);
  check_estop("MANUAL",  vda5050::EStop::MANUAL);
  check_estop("REMOTE",  vda5050::EStop::REMOTE);
  check_estop("NONE",    vda5050::EStop::NONE);
  check_estop("unknown", vda5050::EStop::NONE);  // fallback
}

TEST(RosConverterTest, LoadWithOptionalFieldsRoundTrip) {
  vda5050::Load l;
  l.load_id   = "pallet-1";
  l.load_type = "EUROPALETTE";
  l.weight    = 120.0;

  vda5050::BoundingBoxReference bbr;
  bbr.x = 0.0; bbr.y = 0.0; bbr.z = 0.5; bbr.theta = 0.0;
  l.bounding_box_reference = bbr;

  vda5050::LoadDimensions dims;
  dims.length = 1.2; dims.width = 0.8; dims.height = 0.15;
  l.load_dimensions = dims;

  const auto ros = vda5050_adapter::ros_from_internal(l);

  EXPECT_TRUE(ros.bounding_box_set);
  EXPECT_TRUE(ros.dimensions_set);
  EXPECT_DOUBLE_EQ(ros.bounding_box_reference.z, 0.5);
  EXPECT_DOUBLE_EQ(ros.load_dimensions.length, 1.2);

  // Round-trip back
  const auto rt = vda5050_adapter::internal_from_ros(ros);
  ASSERT_TRUE(rt.bounding_box_reference.has_value());
  ASSERT_TRUE(rt.load_dimensions.has_value());
  EXPECT_DOUBLE_EQ(rt.bounding_box_reference->z, 0.5);
  EXPECT_DOUBLE_EQ(rt.load_dimensions->length, 1.2);
}

TEST(RosConverterTest, LoadWithoutOptionalFieldsHasFalseFlags) {
  vda5050::Load l;
  l.load_id = "bare";
  const auto ros = vda5050_adapter::ros_from_internal(l);

  EXPECT_FALSE(ros.bounding_box_set);
  EXPECT_FALSE(ros.dimensions_set);
}

TEST(RosConverterTest, ErrorRoundTrip) {
  vda5050::Error e;
  e.error_type        = "navigationError";
  e.error_description = "Cannot reach target";
  e.error_hint        = "Check path clearance";
  e.error_level       = vda5050::ErrorLevel::FATAL;
  e.error_references  = {{"orderId", "o1"}, {"nodeId", "n5"}};

  const auto ros = vda5050_adapter::ros_from_internal(e);
  const auto rt  = vda5050_adapter::internal_from_ros(ros);

  EXPECT_EQ(rt.error_type,        e.error_type);
  EXPECT_EQ(rt.error_description, e.error_description);
  EXPECT_EQ(rt.error_hint,        e.error_hint);
  EXPECT_EQ(rt.error_level,       vda5050::ErrorLevel::FATAL);
  ASSERT_EQ(rt.error_references.size(), 2u);
  EXPECT_EQ(rt.error_references[0].reference_key,   "orderId");
  EXPECT_EQ(rt.error_references[0].reference_value, "o1");
}

TEST(RosConverterTest, VelocityRoundTrip) {
  vda5050::Velocity v{1.5, 0.1, 0.3};
  const auto rt = vda5050_adapter::internal_from_ros(
                    vda5050_adapter::ros_from_internal(v));
  EXPECT_DOUBLE_EQ(rt.vx,    1.5);
  EXPECT_DOUBLE_EQ(rt.vy,    0.1);
  EXPECT_DOUBLE_EQ(rt.omega, 0.3);
}

TEST(RosConverterTest, OperatingModeStringMapping) {
  using vda5050_adapter::internal_from_ros_operating_mode;

  EXPECT_EQ(internal_from_ros_operating_mode("AUTOMATIC"),    vda5050::OperatingMode::AUTOMATIC);
  EXPECT_EQ(internal_from_ros_operating_mode("SEMIAUTOMATIC"),vda5050::OperatingMode::SEMIAUTOMATIC);
  EXPECT_EQ(internal_from_ros_operating_mode("MANUAL"),       vda5050::OperatingMode::MANUAL);
  EXPECT_EQ(internal_from_ros_operating_mode("SERVICE"),      vda5050::OperatingMode::SERVICE);
  EXPECT_EQ(internal_from_ros_operating_mode("TEACHIN"),      vda5050::OperatingMode::TEACHIN);
  EXPECT_EQ(internal_from_ros_operating_mode("unknown"),      vda5050::OperatingMode::AUTOMATIC);
}

TEST(RosConverterTest, ActionStatusStringMapping) {
  using vda5050_adapter::internal_from_ros_action_status;

  EXPECT_EQ(internal_from_ros_action_status("WAITING"),       vda5050::ActionStatus::WAITING);
  EXPECT_EQ(internal_from_ros_action_status("INITIALIZING"),  vda5050::ActionStatus::INITIALIZING);
  EXPECT_EQ(internal_from_ros_action_status("RUNNING"),       vda5050::ActionStatus::RUNNING);
  EXPECT_EQ(internal_from_ros_action_status("PAUSED"),        vda5050::ActionStatus::PAUSED);
  EXPECT_EQ(internal_from_ros_action_status("FINISHED"),      vda5050::ActionStatus::FINISHED);
  EXPECT_EQ(internal_from_ros_action_status("FAILED"),        vda5050::ActionStatus::FAILED);
  EXPECT_EQ(internal_from_ros_action_status("other"),         vda5050::ActionStatus::WAITING);
}
