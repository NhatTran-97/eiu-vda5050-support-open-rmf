#include "vda5050_client_adapter/vda5050_node.hpp"
#include "vda5050_client_adapter/json_converter.hpp"
#include "vda5050_client_adapter/ros_converters.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace vda5050_adapter {

namespace {

// Find action parameter (action) by key (key): return value or empty string if not found.
std::string find_action_parameter(const vda5050::Action& action, const std::string& key)
{
  for (const auto& parameter : action.action_parameters)
  {
    if (parameter.key == key) return parameter.value;
  }
  return "";
}

// Build unique error identity (error) from error_type + sorted error_references for dedup. Format: "errorType|key=value|..."
std::string error_identity(const vda5050::Error& error)
{
  std::vector<std::pair<std::string, std::string>> refs;
  refs.reserve(error.error_references.size());
  for (const auto& ref : error.error_references)
  {
    refs.emplace_back(ref.reference_key, ref.reference_value);
  }

  std::sort(refs.begin(), refs.end());

  std::ostringstream oss;
  oss << error.error_type;
  for (const auto& [key, value] : refs)
  {
    oss << '|' << key << '=' << value;
  }
  return oss.str();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

VDA5050Node::VDA5050Node(const rclcpp::NodeOptions& options): rclcpp::Node("vda5050_client_adapter", options)
{
  declare_and_load_parameters();

  state_machine_  = std::make_unique<AdapterStateMachine>();
  order_manager_  = std::make_unique<OrderManager>();
  action_manager_ = std::make_unique<ActionManager>();

  // ── Wire OrderManager callbacks ─────────────────────────────────────────
  order_manager_->set_order_accepted_callback([this](const std::string& oid, uint32_t uid,
           const std::vector<vda5050::Node>& nodes, const std::vector<vda5050::Edge>& edges) 
  {
    on_order_accepted(oid, uid, nodes, edges);
  });
  order_manager_->set_order_cancelled_callback([this](const std::string& oid) 
  { 
    on_order_cancelled(oid); 
  });
  order_manager_->set_new_base_request_callback([this]()
  { 
    publish_state(); 
  });

  // ── Wire ActionManager callbacks ────────────────────────────────────────
  action_manager_->set_execute_callback([this](const vda5050::Action& a) 
    { 
      on_action_execute(a); 
    });
  action_manager_->set_pause_callback([this](const std::string& id) 
    { 
      on_action_pause(id); 
    });
  action_manager_->set_resume_callback([this](const std::string& id) 
    { 
      on_action_resume(id); 
    });
  action_manager_->set_cancel_callback([this](const std::string& id) 
    { 
      on_action_cancel(id); 
    });

  // ROS interfaces must exist first: setup_mqtt()'s inbound callbacks publish through them.
  setup_ros_interfaces();
  setup_mqtt();
  state_machine_->mark_initialized();

  RCLCPP_INFO(get_logger(), "VDA5050 adapter started — broker: %s  AMR: %s/%s",
              broker_url_.c_str(), manufacturer_.c_str(), serial_number_.c_str());
}

VDA5050Node::~VDA5050Node() 
{
  if (state_machine_)
  {
    state_machine_->start_shutdown();
  }
  teardown_mqtt();
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter loading
// ─────────────────────────────────────────────────────────────────────────────

// Declare and load all ROS2 parameters (MQTT config, VDA5050 identity, timing, factsheet).
void VDA5050Node::declare_and_load_parameters() {
  // MQTT
  broker_url_   = declare_parameter<std::string>("mqtt.broker_url",   "tcp://localhost:1883");
  client_id_    = declare_parameter<std::string>("mqtt.client_id",    "vda5050_client_adapter");
  username_     = declare_parameter<std::string>("mqtt.username",     "");
  password_     = declare_parameter<std::string>("mqtt.password",     "");

  // VDA5050 identity
  interface_name_  = declare_parameter<std::string>("vda5050.interface_name",  "TB3");
  manufacturer_    = declare_parameter<std::string>("vda5050.manufacturer",    "ROBOTIS");
  serial_number_   = declare_parameter<std::string>("vda5050.serial_number",   "0001");

  // Timing
  state_publish_interval_  =
    declare_parameter<double>("vda5050.state_publish_interval",  30.0);
  visualization_interval_  =
    declare_parameter<double>("vda5050.visualization_interval",   1.0);
  // Min interval between the extra state republishes on_agv_position() triggers
  // between state_timer_'s regular ticks, throttled so a fast publisher can't flood MQTT.
  position_publish_min_interval_ =
    declare_parameter<double>("vda5050.position_publish_min_interval", 1.0);
  // Max wait for a HARD action's pause confirmation before ActionManager fails it
  // instead of waiting indefinitely (see ActionManager::check_timeouts).
  hard_action_pause_timeout_ =
    declare_parameter<double>("vda5050.hard_action_pause_timeout", 30.0);

  // ── Factsheet: typeSpecification ───────────────────────────────────────────
  declare_parameter<std::string>("factsheet.type_specification.series_name",        "AMR");
  declare_parameter<std::string>("factsheet.type_specification.series_description", "");
  declare_parameter<std::string>("factsheet.type_specification.agv_kinematic",      "DIFF");
  declare_parameter<std::string>("factsheet.type_specification.agv_class",          "CARRIER");
  declare_parameter<double>     ("factsheet.type_specification.max_load_mass",      0.0);
  declare_parameter<std::vector<std::string>>("factsheet.type_specification.localization_types",
                                              std::vector<std::string>{"NATURAL"});
  declare_parameter<std::vector<std::string>>("factsheet.type_specification.navigation_types",
                                              std::vector<std::string>{"AUTONOMOUS"});

  // ── Factsheet: physicalParameters ─────────────────────────────────────────
  declare_parameter<double>("factsheet.physical_parameters.speed_min",        0.0);
  declare_parameter<double>("factsheet.physical_parameters.speed_max",        1.5);
  declare_parameter<double>("factsheet.physical_parameters.acceleration_max", 1.0);
  declare_parameter<double>("factsheet.physical_parameters.deceleration_max", 2.0);
  declare_parameter<double>("factsheet.physical_parameters.height_min",      -1.0); // -1 = not set
  declare_parameter<double>("factsheet.physical_parameters.height_max",      -1.0);
  declare_parameter<double>("factsheet.physical_parameters.width",            0.5);
  declare_parameter<double>("factsheet.physical_parameters.length",           0.8);

  // ── Factsheet: supported action types (used to build protocolFeatures) ────
  declare_parameter<std::vector<std::string>>(
    "factsheet.supported_action_types",
    std::vector<std::string>{"startPause", "stopPause", "cancelOrder", "stateRequest"});

  // Build factsheet once here so it is ready before MQTT connects
  factsheet_ = build_factsheet_from_params();
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT setup / teardown
// ─────────────────────────────────────────────────────────────────────────────

// Create and configure MQTT client: set up brokerURL, will message, subscriptions to order/instantActions.
void VDA5050Node::setup_mqtt()
{
  MqttConfig cfg;
  cfg.broker_url    = broker_url_;
  cfg.client_id     = client_id_;
  cfg.username      = username_;
  cfg.password      = password_;
  cfg.clean_session = false;

  // Last Will → CONNECTIONBROKEN (VDA5050 §7.2)
  cfg.will_topic    = make_topic("connection");
  cfg.will_qos      = 1;
  cfg.will_retained = true;
  {
    vda5050::Connection broken;
    broken.header           = make_header("connection");
    broken.connection_state = vda5050::ConnectionState::CONNECTIONBROKEN;
    nlohmann::json j = broken;
    cfg.will_payload = j.dump();
  }

  mqtt_client_ = std::make_unique<MqttClient>(cfg);

  mqtt_client_->connect([this](bool connected) {
    state_machine_->on_mqtt_connection_changed(connected);
    if (connected) {
      RCLCPP_INFO(get_logger(), "MQTT connected");
      publish_connection(vda5050::ConnectionState::ONLINE);
      publish_factsheet();   // VDA5050 §9.4 — publish once on connect, retained
      publish_state();
    } else {
      RCLCPP_WARN(get_logger(), "MQTT disconnected");
    }
  });

  // Subscribe to inbound topics
  mqtt_client_->subscribe(make_topic("order"), 0,[this](const MqttMessage& m) 
    { 
      on_order_message(m); 
    });

  mqtt_client_->subscribe(make_topic("instantActions"), 0, [this](const MqttMessage& m) 
    { 
      on_instant_actions_message(m); 
    });
}

// Clean up MQTT: publish OFFLINE connection state and disconnect gracefully.
void VDA5050Node::teardown_mqtt() {
  if (mqtt_client_ && mqtt_client_->is_connected())
  {
    publish_connection(vda5050::ConnectionState::OFFLINE);
    mqtt_client_->disconnect(3000);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ROS2 interface setup
// ─────────────────────────────────────────────────────────────────────────────

// Create ROS2 publishers/subscribers for robot interfaces (order, actions, agv state, navigation feedback); set up timers.
void VDA5050Node::setup_ros_interfaces() {
  // ── Publishers to robot ──────────────────────────────────────────────────
  // transient_local: the bridge always gets the latest order even if it subscribes late or restarts.
  order_pub_ = create_publisher<vda5050_msgs::msg::Order>(
    "~/order", rclcpp::QoS(10).transient_local());
  action_execute_pub_ = create_publisher<vda5050_msgs::msg::Action>("~/action_execute", rclcpp::QoS(10));
  action_cancel_pub_ = create_publisher<std_msgs::msg::String>("~/action_cancel", rclcpp::QoS(10));

  // ── Subscribers from robot ───────────────────────────────────────────────
  using std::placeholders::_1;

  agv_pos_sub_ = create_subscription<vda5050_msgs::msg::AgvPosition>(
    "~/agv_position", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_agv_position, this, _1));

  velocity_sub_ = create_subscription<vda5050_msgs::msg::Velocity>(
    "~/velocity", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_velocity, this, _1));

  battery_sub_ = create_subscription<vda5050_msgs::msg::BatteryState>(
    "~/battery_state", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_battery_state, this, _1));

  // transient_local to match the bridge's latched driving/paused publishers, so a
  // subscriber gets the retained state on (re)start, not just the next change.
  driving_sub_ = create_subscription<std_msgs::msg::Bool>(
    "~/driving", rclcpp::QoS(1).transient_local(),
    std::bind(&VDA5050Node::on_driving, this, _1));

  paused_sub_ = create_subscription<std_msgs::msg::Bool>(
    "~/paused", rclcpp::QoS(1).transient_local(),
    std::bind(&VDA5050Node::on_paused, this, _1));

  action_state_sub_ = create_subscription<vda5050_msgs::msg::ActionState>(
    "~/action_state_feedback", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_action_state_feedback, this, _1));

  error_sub_ = create_subscription<vda5050_msgs::msg::Error>(
    "~/error", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_errors, this, _1));

  safety_sub_ = create_subscription<vda5050_msgs::msg::SafetyState>(
    "~/safety_state", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_safety_state, this, _1));

  op_mode_sub_ = create_subscription<std_msgs::msg::String>(
    "~/operating_mode", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_operating_mode, this, _1));

  load_sub_ = create_subscription<vda5050_msgs::msg::Load>(
    "~/load", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_load, this, _1));

  // Navigation feedback from robot
  node_reached_sub_ = create_subscription<vda5050_msgs::msg::NodeState>(
    "~/node_reached", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_node_reached, this, _1));

  edge_entered_sub_ = create_subscription<vda5050_msgs::msg::EdgeState>(
    "~/edge_entered", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_edge_entered, this, _1));

  edge_completed_sub_ = create_subscription<vda5050_msgs::msg::EdgeState>(
    "~/edge_completed", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_edge_completed, this, _1));

  // ── Timers ───────────────────────────────────────────────────────────────
  state_timer_ = create_wall_timer(
    std::chrono::duration<double>(state_publish_interval_), [this]() 
    { 
      publish_state(); 
    });

  visualization_timer_ = create_wall_timer(
    std::chrono::duration<double>(visualization_interval_),[this]()
    {
      publish_visualization();
    });

  action_timeout_timer_ = create_wall_timer(
    std::chrono::seconds(1), [this]() {
      const bool changed = action_manager_->check_timeouts(
        std::chrono::steady_clock::now(),
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(hard_action_pause_timeout_)));
      if (changed) {
        sync_action_blocking();
        publish_state();
      }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT topic helpers
// ─────────────────────────────────────────────────────────────────────────────

// Build VDA5050 MQTT topic string: interface_name/v2/manufacturer/serial_number/suffix.
std::string VDA5050Node::make_topic(const std::string& suffix) const {
  // uagv/v2/manufacturer/serialNumber/topic
  return interface_name_ + "/v2/" + manufacturer_ + "/" + serial_number_ + "/" + suffix;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT → ROS2  (inbound from Master Control)
// ─────────────────────────────────────────────────────────────────────────────

// Parse Order (msg) from MQTT: merge update if same order_id, or start fresh; publish to bridge.
void VDA5050Node::on_order_message(const MqttMessage& msg) {
  vda5050::Order order;
  try {
    auto j = nlohmann::json::parse(msg.payload);
    order  = j.get<vda5050::Order>();
  } catch (const std::exception& e) {
    RCLCPP_ERROR(get_logger(), "Failed to parse Order JSON: %s", e.what());
    return;
  }

  RCLCPP_INFO(get_logger(), "Order received: id=%s updateId=%u",
              order.order_id.c_str(), order.order_update_id);

  OrderAcceptResult result;
  bool order_accepted = false;  // Will be set to result.accepted below
  {
    // Released before publish_state() (which re-acquires snapshot_mutex_) to avoid recursion;
    // must be acquired before state_mutex_ for consistent lock ordering.
    std::lock_guard<std::mutex> snap_lock(snapshot_mutex_);

    const bool replacing_active_order =
      order.order_id != order_manager_->current_order_id() &&
      order_manager_->has_active_order();
    if (replacing_active_order && action_manager_->has_active_order_actions())
    {
      result.accepted = false;
      result.rejection_reason =
        "New order cannot replace the active order while node/edge actions are still active";
    } else {
      result = order_manager_->process_order(order);
    }
    order_accepted = result.accepted;
  }

  if (!order_accepted) {
    RCLCPP_WARN(get_logger(), "Order rejected: %s", result.rejection_reason.c_str());
    vda5050::Error err;
    err.error_type        = "orderError";
    err.error_level       = vda5050::ErrorLevel::WARNING;
    err.error_description = result.rejection_reason;
    err.error_references  = {{"orderId",       order.order_id},
                             {"orderUpdateId", std::to_string(order.order_update_id)}};
    replace_adapter_error(err);
    publish_state();
    return;
  }

  clear_errors_by_type("orderError");
  // A freshly accepted order clears any lingering navigationError/noOrderToCancel from a
  // prior order, since neither has another natural expiry point.
  clear_errors_by_type("navigationError");
  clear_errors_by_type("noOrderToCancel");

  // A new accepted order resolves any cancelOrder still pending, since a cancel immediately
  // followed by a replacement order would otherwise never see the !driving && !order_active
  // state it's waiting for.
  const auto superseded_cancel = state_machine_->take_pending_cancel();
  if (!superseded_cancel.empty()) {
    action_manager_->set_action_finished(
      superseded_cancel, "Order cancelled (superseded by new order)");
    sync_action_blocking();
    RCLCPP_INFO(get_logger(),
                "Resolved pending cancelOrder %s superseded by new order %s",
                superseded_cancel.c_str(), order.order_id.c_str());
  }

  // Publish to robot as ROS2 message
  auto ros_order = ros_from_internal(order);
  order_pub_->publish(ros_order);

  publish_state();
}

// Parse InstantActions (msg) from MQTT and process through ActionManager; publish state update.
void VDA5050Node::on_instant_actions_message(const MqttMessage& msg) {
  vda5050::InstantActions ia;
  try {
    auto j = nlohmann::json::parse(msg.payload);
    ia     = j.get<vda5050::InstantActions>();
  } 
  catch (const std::exception& e)
   {
    RCLCPP_ERROR(get_logger(), "Failed to parse InstantActions JSON: %s", e.what());
    return;
  }

  RCLCPP_INFO(get_logger(), "InstantActions received: count=%zu",
              ia.actions.size());

  // snapshot_mutex_ must not be held here: execute callbacks (e.g. stateRequest) re-acquire
  // it via publish_state()/build_state_snapshot(), which would deadlock.
  action_manager_->process_instant_actions(ia);
  sync_action_blocking();

  publish_state();
}

// Process instant action (action): return true if handled (pause/resume/stateRequest), false to delegate.
bool VDA5050Node::handle_instant_action(const vda5050::Action& action) {
  const auto& type = action.action_type;

  if (type == "cancelOrder") {
    const auto order_id = find_action_parameter(action, "orderId");
    const auto current_order_id = order_manager_->current_order_id();
    const bool has_active_order = order_manager_->has_active_order();
    const bool matches_active_order =
      order_id.empty() || order_id == current_order_id;

    if (!has_active_order || !matches_active_order) 
    {
      vda5050::Error err;
      err.error_type = "noOrderToCancel";
      err.error_level = vda5050::ErrorLevel::WARNING;
      err.error_description = has_active_order
        ? "cancelOrder did not match the active order"
        : "cancelOrder received while no order is active";
      err.error_references = {{"actionId", action.action_id}};
      if (!order_id.empty()) 
      {
        err.error_references.push_back({"orderId", order_id});
      }
      replace_adapter_error(err);
      action_manager_->set_action_failed(action.action_id,
        has_active_order ? "No matching active order to cancel"
                         : "No active order to cancel");
      publish_state();
      return true;
    }

    const auto replaced_cancel = state_machine_->request_cancel(action.action_id);
    if (!replaced_cancel.empty()) {
      action_manager_->set_action_finished(replaced_cancel, "Superseded by newer cancelOrder");
    }
    clear_errors_by_type("noOrderToCancel");
    // Navigation control is unconditional: the driver must be told to stop
    // regardless of whether any node/edge action happens to be active.
    on_action_cancel(action.action_id);
    action_manager_->cancel_all(action.action_id);
    order_manager_->cancel_order(order_id);
    action_manager_->set_action_running(action.action_id);
    sync_action_blocking();
    publish_state();
    if (maybe_complete_pending_control_actions()) 
    {
      publish_state();
    }
    RCLCPP_INFO(get_logger(), "cancelOrder: %s", order_id.c_str());
    return true;
  }

  if (type == "startPause")
   {
    const auto replaced_pending = state_machine_->request_pause(action.action_id);
    if (!replaced_pending.empty()) {
      action_manager_->set_action_finished(replaced_pending, "Superseded by newer startPause");
    }
    // Navigation control is unconditional: the driver must be told to pause
    // regardless of whether any node/edge action happens to be active.
    on_action_pause(action.action_id);
    action_manager_->pause_all(action.action_id);
    action_manager_->set_action_running(action.action_id);
    sync_action_blocking();
    publish_state();
    if (maybe_complete_pending_control_actions()) {
      publish_state();
    }
    RCLCPP_INFO(get_logger(), "startPause");
    return true;
  }

  if (type == "stopPause") 
  {
    const auto replaced_pending = state_machine_->request_resume(action.action_id);
    if (!replaced_pending.empty()) {
      action_manager_->set_action_finished(replaced_pending, "Superseded by newer stopPause");
    }
    // Navigation control is unconditional: the driver must be told to resume
    // regardless of whether any node/edge action happens to be active.
    on_action_resume(action.action_id);
    action_manager_->resume_all(action.action_id);
    action_manager_->set_action_running(action.action_id);
    sync_action_blocking();
    publish_state();
    if (maybe_complete_pending_control_actions()) {
      publish_state();
    }
    RCLCPP_INFO(get_logger(), "stopPause");
    return true;
  }

  if (type == "stateRequest") {
    action_manager_->set_action_running(action.action_id);
    publish_state();
    action_manager_->set_action_finished(action.action_id, "State published");
    sync_action_blocking();
    publish_state();
    return true;
  }

  // Any other action type (e.g. "initPosition") is delegated to the robot driver:
  // returning false makes the caller forward it via ~/action_execute.
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ROS2 → MQTT  (robot → outbound to Master Control)
// ─────────────────────────────────────────────────────────────────────────────

// Build state snapshot from managers and publish to MQTT state topic; no-op if not connected.
void VDA5050Node::publish_state() {
  if (!mqtt_client_ || !mqtt_client_->is_connected()) return;

  auto state = build_state_snapshot();
  nlohmann::json j = state;

  mqtt_client_->publish(make_topic("state"), j.dump(), 0, false);
}

// Publish connection state (conn_state) to MQTT connection topic with QoS 1 retained; always publishes if client exists.
void VDA5050Node::publish_connection(vda5050::ConnectionState conn_state) {
  vda5050::Connection conn;
  conn.header           = make_header("connection");
  conn.connection_state = conn_state;
  nlohmann::json j = conn;
  mqtt_client_->publish(make_topic("connection"), j.dump(), 1, true);
}

// Publish current AGV position and velocity to MQTT visualization topic; no-op if not connected.
void VDA5050Node::publish_visualization() {
  if (!mqtt_client_ || !mqtt_client_->is_connected()) return;

  vda5050::Visualization viz;
  viz.header = make_header("visualization");
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (agv_position_set_) viz.agv_position = agv_position_;
    if (velocity_set_)     viz.velocity     = velocity_;
  }
  nlohmann::json j = viz;
  mqtt_client_->publish(make_topic("visualization"), j.dump(), 0, false);
}

// Publish factsheet to MQTT factsheet topic with QoS 0 retained; called on MQTT connect for discovery by Master Control.
void VDA5050Node::publish_factsheet() 
{
  if (!mqtt_client_ || !mqtt_client_->is_connected()) return;

  // Refresh header (timestamp + headerId) but keep body unchanged
  factsheet_.header = make_header("factsheet");
  nlohmann::json j  = factsheet_;

  // retained=true so Master Control gets it immediately on (re-)subscribe
  // QoS 0 per VDA5050 §6.2 (factsheet is informational, not safety-critical like connection)
  mqtt_client_->publish(make_topic("factsheet"), j.dump(), 0, true);
  RCLCPP_INFO(get_logger(), "Factsheet published");
}

// Build VDA5050 Factsheet from ROS2 parameters (type_specification, physical_parameters, protocol_features).
vda5050::Factsheet VDA5050Node::build_factsheet_from_params() const {
  vda5050::Factsheet fs;

  // ── typeSpecification ──────────────────────────────────────────────────────
  fs.type_specification.series_name        =
    get_parameter("factsheet.type_specification.series_name").as_string();

  fs.type_specification.series_description =
    get_parameter("factsheet.type_specification.series_description").as_string();

  fs.type_specification.agv_kinematic =
    get_parameter("factsheet.type_specification.agv_kinematic").as_string();

  fs.type_specification.agv_class     =
    get_parameter("factsheet.type_specification.agv_class").as_string();

  fs.type_specification.max_load_mass =
    get_parameter("factsheet.type_specification.max_load_mass").as_double();
  {
    auto loc_types = get_parameter("factsheet.type_specification.localization_types").as_string_array();
    fs.type_specification.localization_types =std::vector<std::string>(loc_types.begin(), loc_types.end());
    auto nav_types = get_parameter("factsheet.type_specification.navigation_types").as_string_array();
    fs.type_specification.navigation_types = std::vector<std::string>(nav_types.begin(), nav_types.end());
  }

  // ── physicalParameters ─────────────────────────────────────────────────────
  auto& pp = fs.physical_parameters;
  pp.speed_min        = get_parameter("factsheet.physical_parameters.speed_min").as_double();
  pp.speed_max        = get_parameter("factsheet.physical_parameters.speed_max").as_double();
  pp.acceleration_max = get_parameter("factsheet.physical_parameters.acceleration_max").as_double();
  pp.deceleration_max = get_parameter("factsheet.physical_parameters.deceleration_max").as_double();
  pp.width            = get_parameter("factsheet.physical_parameters.width").as_double();
  pp.length           = get_parameter("factsheet.physical_parameters.length").as_double();

  const double h_min = get_parameter("factsheet.physical_parameters.height_min").as_double();
  const double h_max = get_parameter("factsheet.physical_parameters.height_max").as_double();
  if (h_min >= 0.0) pp.height_min = h_min;
  if (h_max >= 0.0) pp.height_max = h_max;

  // ── protocolLimits: timing (from existing timing params) ───────────────────
  fs.protocol_limits.timing.default_state_interval   = state_publish_interval_;
  fs.protocol_limits.timing.visualization_interval   = visualization_interval_;

  // ── protocolFeatures: agvActions ──────────────────────────────────────────
  // Built-in instant actions are always supported
  struct BuiltinDef {
    std::string              type;
    std::string              description;
    std::vector<std::string> blocking_types;
  };
  static const std::vector<BuiltinDef> builtins = {
    {"startPause",   "Pause AGV movement and all running actions",            {"NONE"}},
    {"stopPause",    "Resume AGV movement and paused actions",                {"NONE"}},
    {"cancelOrder",  "Cancel the current order and clear the route",          {"NONE"}},
    {"stateRequest", "Trigger an immediate state message publication",        {"NONE"}},
    {"initPosition", "Initialize the AGV position on the map",               {"NONE"}},
  };

  auto action_types_param = get_parameter("factsheet.supported_action_types").as_string_array();
  std::vector<std::string> action_types(action_types_param.begin(), action_types_param.end());

  for (const auto& bt : builtins)
   {
    const bool requested = std::find(action_types.begin(),
                                     action_types.end(),
                                     bt.type) != action_types.end();
    if (!requested) continue;

    vda5050::AgvAction a;
    a.action_type        = bt.type;
    a.action_description = bt.description;
    a.action_scopes      = {"INSTANT"};
    a.blocking_types     = bt.blocking_types;
    fs.protocol_features.agv_actions.push_back(std::move(a));
  }

  // Any extra types not in builtins → add with generic scope INSTANT/NODE/EDGE
  for (const auto& t : action_types) {
    const bool is_builtin = std::any_of(builtins.begin(), builtins.end(),[&](const BuiltinDef& b) 
    { 
      return b.type == t; 
      });
    if (is_builtin) continue;

    vda5050::AgvAction a;
    a.action_type    = t;
    a.action_scopes  = {"INSTANT", "NODE", "EDGE"};
    a.blocking_types = {"NONE", "SOFT", "HARD"};
    fs.protocol_features.agv_actions.push_back(std::move(a));
  }

  return fs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Robot feedback subscribers
// ─────────────────────────────────────────────────────────────────────────────

// Receive AGV position (msg): store and throttle state publishes to avoid broker flooding.
void VDA5050Node::on_agv_position(
  const vda5050_msgs::msg::AgvPosition::SharedPtr msg)
{
  bool should_publish = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    agv_position_     = internal_from_ros(*msg);
    agv_position_set_ = true;

    // Throttled to position_publish_min_interval_ so a fast ~agv_position publisher can't flood the broker.
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(now - last_position_publish_).count();
    if (elapsed >= position_publish_min_interval_) {
      last_position_publish_ = now;
      should_publish = true;
    }
  }
  if (should_publish) {
    publish_state();
  }
}

// Receive AGV velocity (msg): store for visualization publication.
void VDA5050Node::on_velocity(
  const vda5050_msgs::msg::Velocity::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  velocity_     = internal_from_ros(*msg);
  velocity_set_ = true;
}

// Receive battery state (msg): store for state publication.
void VDA5050Node::on_battery_state(
  const vda5050_msgs::msg::BatteryState::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  battery_state_ = internal_from_ros(*msg);
}

// Receive driving state (msg): update state machine, check for completed pause/resume control actions.
void VDA5050Node::on_driving(const std_msgs::msg::Bool::SharedPtr msg) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    changed = (driving_ != msg->data);
    driving_ = msg->data;
  }
  state_machine_->on_driver_driving_changed(msg->data);

  const bool completed_control_action = maybe_complete_pending_control_actions();
  if (changed || completed_control_action) {
    publish_state();
  }
}

// Receive paused state (msg): update state machine, check for completed pause control actions.
void VDA5050Node::on_paused(const std_msgs::msg::Bool::SharedPtr msg) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    changed = (paused_ != msg->data);
    paused_ = msg->data;
  }
  state_machine_->on_driver_paused_changed(msg->data);

  const bool completed_control_action = maybe_complete_pending_control_actions();
  if (changed || completed_control_action) {
    publish_state();
  }
}

// Receive action status feedback (msg): update ActionManager with new status; sync blocking state.
void VDA5050Node::on_action_state_feedback(
  const vda5050_msgs::msg::ActionState::SharedPtr msg)
{
  using S = vda5050::ActionStatus;
  auto status = internal_from_ros_action_status(msg->action_status);

  switch (status) {
    case S::RUNNING:   action_manager_->set_action_running(msg->action_id);  break;
    case S::FINISHED:  action_manager_->set_action_finished(msg->action_id,
                         msg->result_description);                            break;
    case S::FAILED:    action_manager_->set_action_failed(msg->action_id,
                         msg->result_description);                            break;
    case S::PAUSED:    action_manager_->set_action_paused(msg->action_id);   break;
    default: break;
  }
  sync_action_blocking();
  publish_state();
}

// Receive error from driver (msg): upsert into error list; publish state immediately (VDA5050 §7.3).
void VDA5050Node::on_errors(const vda5050_msgs::msg::Error::SharedPtr msg) {
  upsert_driver_error(internal_from_ros(*msg));
  // Publish state immediately on any new error (VDA5050 §7.3)
  publish_state();
}

// Receive safety state (msg): store for state publication.
void VDA5050Node::on_safety_state(
  const vda5050_msgs::msg::SafetyState::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  safety_state_ = internal_from_ros(*msg);
}

// Receive operating mode (msg): store and convert from ROS2 string representation.
void VDA5050Node::on_operating_mode(
  const std_msgs::msg::String::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  operating_mode_ = internal_from_ros_operating_mode(msg->data);
}

// Receive load state (msg): store for state publication.
void VDA5050Node::on_load(const vda5050_msgs::msg::Load::SharedPtr msg) 
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = std::find_if(loads_.begin(), loads_.end(),[&](const vda5050::Load& l) 
    { 
      return l.load_id == msg->load_id; 
    });
  if (it != loads_.end()) *it = internal_from_ros(*msg);
  else loads_.push_back(internal_from_ros(*msg));
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation feedback from robot
// ─────────────────────────────────────────────────────────────────────────────

// Receive node reached notification (msg): validate order progression, trigger node actions, sync state.
void VDA5050Node::on_node_reached(
  const vda5050_msgs::msg::NodeState::SharedPtr msg)
{
  RCLCPP_INFO(get_logger(), "Node reached: %s (seq=%u)",msg->node_id.c_str(), msg->sequence_id);

  NodeReachedEvent evt;
  evt.node_id          = msg->node_id;
  evt.sequence_id      = msg->sequence_id;
  evt.distance_driven  = msg->distance_driven;

  if (!order_manager_->node_reached(evt))
  {
    vda5050::Error err;
    err.error_type = "navigationOrderError";
    err.error_level = vda5050::ErrorLevel::WARNING;
    err.error_description = "Received node_reached out of order";
    err.error_references = {{"eventType", "node_reached"},
                            {"nodeId", msg->node_id},
                            {"sequenceId", std::to_string(msg->sequence_id)}};
    replace_adapter_error(err);
    publish_state();
    return;
  }

  clear_errors_by_type("navigationOrderError");

  // Trigger actions for this node
  action_manager_->on_node_reached(msg->node_id, msg->sequence_id);
  sync_order_activity();
  sync_action_blocking();

  publish_state();
}

// Receive edge entered notification (msg): validate order, trigger edge actions, sync state.
void VDA5050Node::on_edge_entered(
  const vda5050_msgs::msg::EdgeState::SharedPtr msg)
{
  RCLCPP_INFO(get_logger(), "Edge entered: %s (seq=%u)",
              msg->edge_id.c_str(), msg->sequence_id);

  if (!order_manager_->edge_entered(msg->edge_id, msg->sequence_id)) {
    vda5050::Error err;
    err.error_type = "navigationOrderError";
    err.error_level = vda5050::ErrorLevel::WARNING;
    err.error_description = "Received edge_entered out of order";
    err.error_references = {{"eventType", "edge_entered"},
                            {"edgeId", msg->edge_id},
                            {"sequenceId", std::to_string(msg->sequence_id)}};
    replace_adapter_error(err);
    publish_state();
    return;
  }

  clear_errors_by_type("navigationOrderError");

  action_manager_->on_edge_entered(msg->edge_id, msg->sequence_id);
  sync_action_blocking();
  publish_state();
}

// Receive edge completed notification (msg): validate order, fail uncompleted edge actions, sync state.
void VDA5050Node::on_edge_completed(
  const vda5050_msgs::msg::EdgeState::SharedPtr msg)
{
  RCLCPP_INFO(get_logger(), "Edge completed: %s (seq=%u)",
              msg->edge_id.c_str(), msg->sequence_id);

  if (!order_manager_->edge_completed(msg->edge_id, msg->sequence_id)) {
    vda5050::Error err;
    err.error_type = "navigationOrderError";
    err.error_level = vda5050::ErrorLevel::WARNING;
    err.error_description = "Received edge_completed out of order";
    err.error_references = {{"eventType", "edge_completed"},
                            {"edgeId", msg->edge_id},
                            {"sequenceId", std::to_string(msg->sequence_id)}};
    replace_adapter_error(err);
    publish_state();
    return;
  }

  clear_errors_by_type("navigationOrderError");
  action_manager_->on_edge_left(msg->edge_id, msg->sequence_id);
  sync_order_activity();
  sync_action_blocking();
  publish_state();
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderManager callbacks
// ─────────────────────────────────────────────────────────────────────────────

// OrderManager callback: order was accepted. Reset action state for new order, publish to robot.
void VDA5050Node::on_order_accepted(
  const std::string& order_id,
  uint32_t order_update_id,
  const std::vector<vda5050::Node>& remaining_nodes,
  const std::vector<vda5050::Edge>& remaining_edges)
{
  RCLCPP_INFO(get_logger(), "Order accepted: %s (updateId=%u, nodes=%zu, edges=%zu)",
              order_id.c_str(), order_update_id,
              remaining_nodes.size(), remaining_edges.size());

  if (order_id != action_state_order_id_) 
  {
    action_manager_->reset_for_new_order();
    action_state_order_id_ = order_id;
  }

  action_manager_->sync_order_actions(remaining_nodes, remaining_edges);
  sync_order_activity();
  sync_action_blocking();
}

// OrderManager callback: order cancelled. Sync order/action state.
void VDA5050Node::on_order_cancelled(const std::string& order_id)
{
  RCLCPP_INFO(get_logger(), "Order cancelled: %s", order_id.c_str());
  sync_order_activity();
  sync_action_blocking();
  publish_state();
}

// ─────────────────────────────────────────────────────────────────────────────
// ActionManager callbacks
// ─────────────────────────────────────────────────────────────────────────────

// ActionManager callback: action (action) ready to execute. Try instant actions first, else publish to robot driver.
void VDA5050Node::on_action_execute(const vda5050::Action& action)
{
  RCLCPP_INFO(get_logger(), "Execute action: type=%s id=%s",
              action.action_type.c_str(), action.action_id.c_str());

  if (handle_instant_action(action)) return;

  auto ros_action = ros_from_internal(action);
  action_execute_pub_->publish(ros_action);
  publish_state();
}

// ActionManager callback: pause action (action_id). Publish pause signal to robot driver via action_cancel topic.
void VDA5050Node::on_action_pause(const std::string& action_id)
{
  RCLCPP_INFO(get_logger(), "Pause action: %s", action_id.c_str());
  // Publish cancel/pause signal to robot driver
  std_msgs::msg::String msg;
  msg.data = "pause:" + action_id;
  action_cancel_pub_->publish(msg);
}

// ActionManager callback: resume action (action_id). Publish resume signal to robot driver via action_cancel topic.
void VDA5050Node::on_action_resume(const std::string& action_id)
{
  RCLCPP_INFO(get_logger(), "Resume action: %s", action_id.c_str());
  std_msgs::msg::String msg;
  msg.data = "resume:" + action_id;
  action_cancel_pub_->publish(msg);
}

// ActionManager callback: cancel action (action_id). Publish cancel signal to robot driver via action_cancel topic.
void VDA5050Node::on_action_cancel(const std::string& action_id)
{
  RCLCPP_INFO(get_logger(), "Cancel action: %s", action_id.c_str());
  std_msgs::msg::String msg;
  msg.data = "cancel:" + action_id;
  action_cancel_pub_->publish(msg);
  publish_state();
}

// Check if any control actions (pause/resume/cancel) completed; mark them finished in ActionManager.
bool VDA5050Node::maybe_complete_pending_control_actions()
{
  const auto completed_actions = state_machine_->consume_ready_control_actions();

  for (const auto& completed : completed_actions) {
    action_manager_->set_action_finished(completed.action_id,
                                         completed.result_description);
  }

  if (!completed_actions.empty()) {
    sync_action_blocking();
  }

  return !completed_actions.empty();
}

// Replace adapter error (error) of same type; recompute fatal error state for state machine.
void VDA5050Node::replace_adapter_error(const vda5050::Error& error) 
{
  bool has_fatal_error = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    errors_.erase(
      std::remove_if(errors_.begin(), errors_.end(), [&](const vda5050::Error& existing) 
      {
        return existing.error_type == error.error_type;
      }),
      errors_.end());
    errors_.push_back(error);
    has_fatal_error = std::any_of(errors_.begin(), errors_.end(), [](const vda5050::Error& entry) {
      return entry.error_level == vda5050::ErrorLevel::FATAL;
    });
  }
  state_machine_->on_fatal_error_changed(has_fatal_error);
}

// Upsert driver error (error) into list (merge by identity, skip adapter-owned error types); recompute fatal status.
void VDA5050Node::upsert_driver_error(const vda5050::Error& error)
{
  bool has_fatal_error = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto identity = error_identity(error);
    auto it = std::find_if(errors_.begin(), errors_.end(),
                           [&](const vda5050::Error& existing)
                           {
                             return existing.error_type != "orderError" &&
                                    existing.error_type != "noOrderToCancel" &&
                                    existing.error_type != "navigationOrderError" &&
                                    error_identity(existing) == identity;
                           });
    if (it != errors_.end())
    {
      *it = error;
    }
    else
    {
      errors_.push_back(error);
    }
    has_fatal_error = std::any_of(errors_.begin(), errors_.end(), [](const vda5050::Error& entry) {
      return entry.error_level == vda5050::ErrorLevel::FATAL;
    });
  }
  state_machine_->on_fatal_error_changed(has_fatal_error);
}

// Clear all errors of type (error_type) from list; recompute fatal error state for state machine.
void VDA5050Node::clear_errors_by_type(const std::string& error_type)
{
  bool has_fatal_error = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    errors_.erase(
      std::remove_if(errors_.begin(), errors_.end(),
                     [&](const vda5050::Error& error) {
                       return error.error_type == error_type;
                     }),
      errors_.end());
    has_fatal_error = std::any_of(errors_.begin(), errors_.end(), [](const vda5050::Error& entry) {
      return entry.error_level == vda5050::ErrorLevel::FATAL;
    });
  }
  state_machine_->on_fatal_error_changed(has_fatal_error);
}

// Sync order activity state: notify state machine if order is active or not.
void VDA5050Node::sync_order_activity()
{
  state_machine_->on_order_state_changed(order_manager_->has_active_order());
}

// Sync action blocking state: notify state machine if any HARD/SOFT action is blocking.
void VDA5050Node::sync_action_blocking()
{
  const bool action_blocked =
    action_manager_->is_hard_blocked() || action_manager_->is_soft_blocked();
  state_machine_->on_action_blocking_changed(action_blocked);
}

// ─────────────────────────────────────────────────────────────────────────────
// State snapshot
// ─────────────────────────────────────────────────────────────────────────────

// Build complete VDA5050 State message from order, action, and robot state snapshots.
vda5050::State VDA5050Node::build_state_snapshot() const
{
  vda5050::State s;
  s.header = make_header("state");

  // Serializes against on_order_message/on_instant_actions_message so order_manager_/action_manager_
  // aren't mutated mid-snapshot; must be acquired before state_mutex_ (consistent ordering).
  {
    std::lock_guard<std::mutex> snap_lock(snapshot_mutex_);

    s.order_id               = order_manager_->current_order_id();
    s.order_update_id        = order_manager_->current_order_update_id();
    s.zone_set_id            = order_manager_->current_zone_set_id();
    s.last_node_id           = order_manager_->last_node_id();
    s.last_node_sequence_id  = order_manager_->last_node_sequence_id();
    s.node_states            = order_manager_->node_states();
    s.edge_states            = order_manager_->edge_states();
    s.new_base_request       = order_manager_->new_base_request();
    s.distance_since_last_node = order_manager_->distance_since_last_node();

    s.action_states = action_manager_->action_states();
    s.driving       = state_machine_->reported_driving();
    s.paused        = state_machine_->paused();
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (agv_position_set_) s.agv_position = agv_position_;
    if (velocity_set_)     s.velocity     = velocity_;
    s.battery_state         = battery_state_;
    s.safety_state          = safety_state_;
    s.loads                 = loads_;
    s.errors                = errors_;
    s.operating_mode        = operating_mode_;
  }

  return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Header factory
// ─────────────────────────────────────────────────────────────────────────────

// Build VDA5050 header for message topic (topic): increment per-topic header_id, set timestamp/version/manufacturer/serial.
vda5050::Header VDA5050Node::make_header(const std::string& topic) const
 {
  std::lock_guard<std::mutex> lock(hdr_mutex_);
  vda5050::Header h;
  h.header_id     = ++header_ids_[topic];
  h.timestamp     = now_iso8601();
  h.version       = "2.1.0";
  h.manufacturer  = manufacturer_;
  h.serial_number = serial_number_;
  return h;
}

// Return current time as ISO8601 UTC string with millisecond precision (e.g., "2026-09-03T12:34:56.789Z").
std::string VDA5050Node::now_iso8601()
 {
  using Clock = std::chrono::system_clock;
  auto now    = Clock::now();
  auto t      = Clock::to_time_t(now);
  auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
  std::ostringstream oss;
  std::tm tm_buf{};
  gmtime_r(&t, &tm_buf);
  oss << std::put_time(&tm_buf, "%FT%T")
      << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
  return oss.str();
}

}  // namespace vda5050_adapter
