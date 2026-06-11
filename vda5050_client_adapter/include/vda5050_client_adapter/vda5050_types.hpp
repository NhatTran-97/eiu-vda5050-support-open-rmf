#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vda5050 {

// ─────────────────────────────────────────────────────────────────────────────
// Enumerations
// ─────────────────────────────────────────────────────────────────────────────

enum class BlockingType { NONE, SOFT, HARD };
enum class ActionStatus {
  WAITING,
  INITIALIZING,
  RUNNING,
  PAUSED,
  FINISHED,
  FAILED
};
enum class EStop { AUTOACK, MANUAL, REMOTE, NONE };
enum class OperatingMode {
  AUTOMATIC,
  SEMIAUTOMATIC,
  MANUAL,
  SERVICE,
  TEACHIN
};
enum class ConnectionState { ONLINE, OFFLINE, CONNECTIONBROKEN };
enum class ErrorLevel { WARNING, FATAL };
enum class InfoLevel { DEBUG, INFO };
enum class MapStatus { ENABLED, DISABLED };

// ─────────────────────────────────────────────────────────────────────────────
// Header
// ─────────────────────────────────────────────────────────────────────────────

struct Header {
  uint32_t    header_id{0};
  std::string timestamp;    // ISO-8601 e.g. "2024-01-01T00:00:00.000Z"
  std::string version{"2.1.0"};
  std::string manufacturer;
  std::string serial_number;
};

// ─────────────────────────────────────────────────────────────────────────────
// Actions
// ─────────────────────────────────────────────────────────────────────────────

struct ActionParameter {
  std::string key;
  std::string value;  // JSON-encoded; strings, numbers, booleans, arrays, objects
};

struct Action {
  std::string                   action_type;
  std::string                   action_id;
  std::string                   action_description;
  BlockingType                  blocking_type{BlockingType::NONE};
  std::vector<ActionParameter>  action_parameters;
};

// ─────────────────────────────────────────────────────────────────────────────
// Graph elements
// ─────────────────────────────────────────────────────────────────────────────

struct NodePosition {
  double      x{0.0};
  double      y{0.0};
  double      theta{0.0};
  bool        theta_set{false};
  double      allowed_deviation_xy{0.0};
  double      allowed_deviation_theta{0.0};
  std::string map_id;
  std::string map_description;
};

struct Node {
  std::string              node_id;
  uint32_t                 sequence_id{0};
  std::string              node_description;
  bool                     released{false};
  std::optional<NodePosition> node_position;
  std::vector<Action>      actions;
};

struct ControlPoint {
  double x{0.0};
  double y{0.0};
  double weight{1.0};
};

struct Trajectory {
  double                    degree{0.0};
  std::vector<double>       knot_vector;
  std::vector<ControlPoint> control_points;
};

struct Corridor {
  double      left_width{0.0};
  double      right_width{0.0};
  std::string corridor_ref_point;  // KINEMATICCENTER | CONTOUR
};

struct Edge {
  std::string             edge_id;
  uint32_t                sequence_id{0};
  std::string             edge_description;
  bool                    released{false};
  std::string             start_node_id;
  std::string             end_node_id;
  double                  max_speed{-1.0};        // -1 = not set
  double                  max_height{-1.0};
  double                  min_height{-1.0};
  std::optional<double>   orientation;
  std::string             orientation_type;       // GLOBAL | TANGENTIAL
  std::string             direction;              // LEFT | RIGHT | STRAIGHT | ANY
  std::optional<bool>     rotation_allowed;
  double                  max_rotation_speed{-1.0};
  std::optional<Trajectory> trajectory;
  double                  length{-1.0};
  std::optional<Corridor> corridor;
  std::vector<Action>     actions;
};

// ─────────────────────────────────────────────────────────────────────────────
// Order / InstantActions
// ─────────────────────────────────────────────────────────────────────────────

struct Order {
  Header              header;
  std::string         order_id;
  uint32_t            order_update_id{0};
  std::string         zone_set_id;
  std::vector<Node>   nodes;
  std::vector<Edge>   edges;
};

struct InstantActions {
  Header              header;
  std::vector<Action> actions;
};

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

struct NodeState {
  std::string              node_id;
  uint32_t                 sequence_id{0};
  std::string              node_description;
  bool                     released{false};
  std::optional<NodePosition> node_position;
};

struct EdgeState {
  std::string             edge_id;
  uint32_t                sequence_id{0};
  std::string             edge_description;
  bool                    released{false};
  std::optional<Trajectory> trajectory;
};

struct AgvPosition {
  bool        position_initialized{false};
  double      localization_score{-1.0};  // -1 = not set
  double      deviation_range{-1.0};
  double      x{0.0};
  double      y{0.0};
  double      theta{0.0};
  std::string map_id;
  std::string map_description;
};

struct Velocity {
  double vx{0.0};
  double vy{0.0};
  double omega{0.0};
};

struct BoundingBoxReference {
  double x{0.0};
  double y{0.0};
  double z{0.0};
  double theta{0.0};
};

struct LoadDimensions {
  double length{-1.0};
  double width{-1.0};
  double height{-1.0};
};

struct Load {
  std::string                    load_id;
  std::string                    load_type;
  std::string                    load_position;
  std::optional<BoundingBoxReference> bounding_box_reference;
  std::optional<LoadDimensions>  load_dimensions;
  double                         weight{-1.0};
};

struct ActionState {
  std::string  action_id;
  std::string  action_type;
  std::string  action_description;
  ActionStatus action_status{ActionStatus::WAITING};
  std::string  result_description;
};

struct BatteryState {
  double   battery_charge{0.0};   // %
  double   battery_voltage{-1.0}; // V; -1 = not set
  int      battery_health{-1};    // 0-100; -1 = not set
  bool     charging{false};
  uint32_t reach{0};              // meters; 0 = not set
};

struct ErrorReference {
  std::string reference_key;
  std::string reference_value;
};

struct Error {
  std::string                  error_type;
  std::vector<ErrorReference>  error_references;
  std::string                  error_description;
  std::string                  error_hint;
  ErrorLevel                   error_level{ErrorLevel::WARNING};
};

struct InfoReference {
  std::string reference_key;
  std::string reference_value;
};

struct Info {
  std::string                info_type;
  std::vector<InfoReference> info_references;
  std::string                info_description;
  InfoLevel                  info_level{InfoLevel::INFO};
};

struct SafetyState {
  EStop e_stop{EStop::NONE};
  bool  field_violation{false};
};

struct MapInfo {
  std::string map_id;
  std::string map_version;
  std::string map_description;
  MapStatus   map_status{MapStatus::ENABLED};
};

struct State {
  Header                  header;
  std::string             order_id;
  uint32_t                order_update_id{0};
  std::string             zone_set_id;
  std::string             last_node_id;
  uint32_t                last_node_sequence_id{0};
  std::vector<NodeState>  node_states;
  std::vector<EdgeState>  edge_states;
  std::optional<AgvPosition> agv_position;
  std::optional<Velocity> velocity;
  std::vector<Load>       loads;
  bool                    driving{false};
  bool                    paused{false};
  bool                    new_base_request{false};
  double                  distance_since_last_node{0.0};
  std::vector<ActionState> action_states;
  BatteryState            battery_state;
  OperatingMode           operating_mode{OperatingMode::AUTOMATIC};
  std::vector<Error>      errors;
  std::vector<Info>       information;
  SafetyState             safety_state;
  std::vector<MapInfo>    maps;
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection / Visualization
// ─────────────────────────────────────────────────────────────────────────────

struct Connection {
  Header          header;
  ConnectionState connection_state{ConnectionState::ONLINE};
};

struct Visualization {
  Header                  header;
  std::optional<AgvPosition> agv_position;
  std::optional<Velocity> velocity;
};

// ─────────────────────────────────────────────────────────────────────────────
// Factsheet  (VDA5050 §9.4)
// ─────────────────────────────────────────────────────────────────────────────

/** Physical and kinematic description of the AGV model. */
struct TypeSpecification {
  std::string              series_name;
  std::string              series_description;        // optional
  std::string              agv_kinematic;             // "DIFF" | "OMNI" | "THREEWHEEL"
  std::string              agv_class;                 // "FORKLIFT" | "CONVEYOR" | "TUGGER" | "CARRIER"
  double                   max_load_mass{0.0};        // kg
  std::vector<std::string> localization_types;        // e.g. {"NATURAL", "REFLECTOR"}
  std::vector<std::string> navigation_types;          // e.g. {"AUTONOMOUS"}
};

/** Motion envelope and limits. */
struct PhysicalParameters {
  double speed_min{0.0};          // m/s — minimum controllable speed
  double speed_max{1.5};          // m/s — maximum speed
  double acceleration_max{1.0};   // m/s²
  double deceleration_max{2.0};   // m/s²
  std::optional<double> height_min;   // m — min vehicle height (optional)
  std::optional<double> height_max;   // m — max vehicle height (optional)
  double width{0.5};              // m
  double length{0.8};             // m
};

/** Protocol-level limits declared by the AGV (all optional). */
struct MaxStringLens {
  std::optional<uint32_t> msg_len;
  std::optional<uint32_t> topic_serial_len;
  std::optional<uint32_t> topic_elem_len;
  std::optional<uint32_t> id_len;
  std::optional<bool>     id_numerical_only;
  std::optional<uint32_t> enum_len;
  std::optional<uint32_t> load_id_len;
};

// JSON keys use dot-notation per VDA5050 §9.4 (e.g. "order.nodes", "state.errors")
struct MaxArrayLens {
  // Order
  std::optional<uint32_t> order_nodes;               // "order.nodes"
  std::optional<uint32_t> order_edges;               // "order.edges"
  std::optional<uint32_t> node_actions;              // "node.actions"
  std::optional<uint32_t> edge_actions;              // "edge.actions"
  std::optional<uint32_t> actions_action_parameters; // "actions.actionsParameters"
  std::optional<uint32_t> instant_actions;           // "instantActions"
  // Trajectory
  std::optional<uint32_t> trajectory_knot_vector;    // "trajectory.knotVector"
  std::optional<uint32_t> trajectory_control_points; // "trajectory.controlPoints"
  // State
  std::optional<uint32_t> state_node_states;         // "state.nodeStates"
  std::optional<uint32_t> state_edge_states;         // "state.edgeStates"
  std::optional<uint32_t> state_loads;               // "state.loads"
  std::optional<uint32_t> state_action_states;       // "state.actionStates"
  std::optional<uint32_t> state_errors;              // "state.errors"
  std::optional<uint32_t> state_information;         // "state.information"
  // Error / Info
  std::optional<uint32_t> error_error_references;    // "error.errorReferences"
  std::optional<uint32_t> info_info_references;      // "information.infoReferences"
};

struct TimingLimits {
  std::optional<double> min_order_interval;       // s
  std::optional<double> min_state_interval;       // s
  std::optional<double> default_state_interval;   // s
  std::optional<double> visualization_interval;   // s
};

struct ProtocolLimits {
  MaxStringLens max_string_lens;
  MaxArrayLens  max_array_lens;
  TimingLimits  timing;
};

/** An optional parameter with its support status. */
struct OptionalParameter {
  std::string parameter;               // dot-path, e.g. "order.nodes[].nodePosition.mapId"
  std::string support;                 // "SUPPORTED" | "REQUIRED"
  std::string description;             // human-readable (optional)
};

/** Definition of one parameter for an AGV action. */
struct AgvActionParameter {
  std::string key;
  std::string value_data_type;  // "BOOL"|"NUMBER"|"INTEGER"|"FLOAT"|"STRING"|"OBJECT"|"ARRAY"
  std::string description;
  bool        is_optional{true};
};

/** An action type supported by the AGV. */
struct AgvAction {
  std::string                      action_type;
  std::vector<std::string>         action_scopes;     // "INSTANT" | "NODE" | "EDGE"
  std::string                      action_description;
  std::vector<AgvActionParameter>  action_parameters;
  std::string                      result_description; // free text description of result
  std::vector<std::string>         blocking_types;    // "NONE" | "SOFT" | "HARD"
};

/** AGV protocol capabilities. */
struct ProtocolFeatures {
  std::vector<OptionalParameter> optional_parameters;
  std::vector<AgvAction>         agv_actions;
};

/** Root factsheet document — published once to the "factsheet" MQTT topic. */
struct Factsheet {
  Header             header;
  TypeSpecification  type_specification;
  PhysicalParameters physical_parameters;
  ProtocolLimits     protocol_limits;
  ProtocolFeatures   protocol_features;
};

}  // namespace vda5050
