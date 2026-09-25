#include "vda5050_client_adapter/vda5050_node.hpp"
#include "vda5050_client_adapter/json_converter.hpp"
#include "vda5050_client_adapter/ros_converters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace vda5050_adapter {

namespace {

// Error types only the adapter raises and clears; driver errors never replace them.
constexpr std::array<const char*, 5> kAdapterErrorTypes = {
  "orderError", "orderUpdateError", "validationError", "noOrderToCancel", "driverConnectionError"};

// Raised while the robot driver is lost (driver_status liveliness).
constexpr char kDriverLostError[] = "driverConnectionError";

// Reference key naming the MQTT topic of a validationError.
constexpr char kTopicReference[] = "topic";

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

bool is_adapter_error_type(const std::string& type)
{
  return std::find(kAdapterErrorTypes.begin(), kAdapterErrorTypes.end(), type) != kAdapterErrorTypes.end();
}

// Return the reference value for key (key) of error (error), or empty.
std::string error_reference(const vda5050::Error& error, const std::string& key)
{
  for (const auto& ref : error.error_references)
  {
    if (ref.reference_key == key) return ref.reference_value;
  }
  return "";
}

// Same route step: order, node and sequence id.
bool same_step(const RouteStep& a, const RouteStep& b)
{
  return a.order_id == b.order_id && a.node.node_id == b.node.node_id && a.node.sequence_id == b.node.sequence_id;
}

// Throw std::invalid_argument naming the parameter (name) unless ok.
void require(bool ok, const std::string& name, const std::string& rule)
{
  if (!ok) throw std::invalid_argument("Parameter '" + name + "' must be " + rule);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

VDA5050Node::VDA5050Node(const rclcpp::NodeOptions& options): rclcpp::Node("vda5050_client_adapter", options)
{
  declare_and_load_parameters();

  state_machine_  = std::make_unique<AdapterStateMachine>();
  order_manager_  = std::make_unique<OrderManager>();
  action_manager_ = std::make_unique<ActionManager>();

  state_machine_->set_report_actual_driving(strict_mode_);
  order_manager_->set_strict_mode(strict_mode_);
  order_manager_->set_new_base_request_min_base_nodes(
    static_cast<std::size_t>(new_base_request_min_base_nodes_));
  action_manager_->set_sequential_hard_actions(strict_mode_);
  action_manager_->set_max_finished_instant_actions(
    static_cast<std::size_t>(max_finished_instant_actions_));

  action_manager_->set_control_action_types(
    {"cancelOrder", "startPause", "stopPause", "stateRequest", "factsheetRequest"});

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

  RCLCPP_INFO(get_logger(), "VDA5050 adapter started — broker: %s  AMR: %s/%s  client_id: %s%s",
              broker_url_.c_str(), manufacturer_.c_str(), serial_number_.c_str(), client_id_.c_str(),
              strict_mode_ ? "" : "  (strict_mode off: legacy order handling)");
}

VDA5050Node::~VDA5050Node()
{
  if (state_machine_)
  {
    state_machine_->start_shutdown();
  }
  teardown_mqtt();
  // Destroy the MQTT client before the other members.
  mqtt_client_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter loading
// ─────────────────────────────────────────────────────────────────────────────

// Declare, load and validate all ROS2 parameters (MQTT config, VDA5050 identity, timing, factsheet).
void VDA5050Node::declare_and_load_parameters() {
  // MQTT
  broker_url_   = declare_parameter<std::string>("mqtt.broker_url",   "tcp://localhost:1883");
  client_id_    = declare_parameter<std::string>("mqtt.client_id",    "");
  username_     = declare_parameter<std::string>("mqtt.username",     "");
  password_     = declare_parameter<std::string>("mqtt.password",     "");

  // VDA5050 identity
  interface_name_  = declare_parameter<std::string>("vda5050.interface_name",  "TB3");
  manufacturer_    = declare_parameter<std::string>("vda5050.manufacturer",    "ROBOTIS");
  serial_number_   = declare_parameter<std::string>("vda5050.serial_number",   "0001");

  // Empty client_id: derived from the AGV identity.
  if (client_id_.empty())
  {
    client_id_ = "vda5050_" + interface_name_ + "_" + manufacturer_ + "_" + serial_number_;
  }

  // Timing
  state_publish_interval_  =  declare_parameter<double>("vda5050.state_publish_interval",  30.0);
  visualization_interval_  =  declare_parameter<double>("vda5050.visualization_interval",   1.0);
  // Min interval between the extra state republishes on_agv_position() triggers
  // between state_timer_'s regular ticks, throttled so a fast publisher can't flood MQTT.
  position_publish_min_interval_ =  declare_parameter<double>("vda5050.position_publish_min_interval", 1.0);
  // Max wait for a HARD action's pause confirmation before ActionManager fails it
  // instead of waiting indefinitely (see ActionManager::check_timeouts).
  hard_action_pause_timeout_ =  declare_parameter<double>("vda5050.hard_action_pause_timeout", 30.0);
  // Period of the executor tick that runs queued MQTT work and publishes a changed state.
  event_loop_period_ = declare_parameter<double>("vda5050.event_loop_period", 0.01);

  // Behavior
  strict_mode_ = declare_parameter<bool>("vda5050.strict_mode", true);
  new_base_request_min_base_nodes_ =  declare_parameter<int64_t>("vda5050.new_base_request_min_base_nodes", 2);
  max_finished_instant_actions_ =  declare_parameter<int64_t>("vda5050.max_finished_instant_actions", 50);
  driver_status_max_lease_ = declare_parameter<double>("vda5050.driver_status_max_lease", 10.0);

  require(!broker_url_.empty(), "mqtt.broker_url", "non-empty");
  require(!interface_name_.empty() && !manufacturer_.empty() && !serial_number_.empty(),
          "vda5050.interface_name/manufacturer/serial_number", "non-empty");
  require(state_publish_interval_ > 0.0, "vda5050.state_publish_interval", "> 0");
  require(visualization_interval_ > 0.0, "vda5050.visualization_interval", "> 0");
  require(position_publish_min_interval_ >= 0.0, "vda5050.position_publish_min_interval", ">= 0");
  require(hard_action_pause_timeout_ > 0.0, "vda5050.hard_action_pause_timeout", "> 0");
  require(event_loop_period_ > 0.0 && event_loop_period_ <= 1.0, "vda5050.event_loop_period", "in (0, 1] s");
  require(new_base_request_min_base_nodes_ >= 1, "vda5050.new_base_request_min_base_nodes", ">= 1");
  require(max_finished_instant_actions_ >= 0, "vda5050.max_finished_instant_actions", ">= 0");
  require(driver_status_max_lease_ >= 0.0, "vda5050.driver_status_max_lease", ">= 0");

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
  declare_parameter<std::vector<std::string>>( "factsheet.supported_action_types",
    std::vector<std::string>{"startPause", "stopPause", "cancelOrder", "stateRequest", "factsheetRequest"});

  // Build factsheet once here so it is ready before MQTT connects
  factsheet_ = build_factsheet_from_params();
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT setup / teardown
// ─────────────────────────────────────────────────────────────────────────────

// Create and configure MQTT client: set up brokerURL, will message, subscriptions to order/instantActions.
// Every MQTT callback only queues its work for the executor thread.
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

  // Register subscriptions before connecting.
  mqtt_client_->subscribe(make_topic("order"), 0, [this](const MqttMessage& m)
    {
      post([this, m] { on_order_message(m); });
    });

  mqtt_client_->subscribe(make_topic("instantActions"), 0, [this](const MqttMessage& m)
    {
      post([this, m] { on_instant_actions_message(m); });
    });

  mqtt_client_->connect([this](bool connected)
    {
      post([this, connected] { on_mqtt_connection_changed(connected); });
    });
}

// Clean up MQTT: publish OFFLINE connection state and disconnect gracefully.
void VDA5050Node::teardown_mqtt() {
  if (mqtt_client_ && mqtt_client_->is_connected())
  {
    publish_connection(vda5050::ConnectionState::OFFLINE);
  }
  if (mqtt_client_)
  {
    mqtt_client_->disconnect(3000);
  }
}

// Record the MQTT connection (connected); on connect publish connection, factsheet and state.
void VDA5050Node::on_mqtt_connection_changed(bool connected)
{
  state_machine_->on_mqtt_connection_changed(connected);
  if (connected) {
    RCLCPP_INFO(get_logger(), "MQTT connected");
    publish_connection(vda5050::ConnectionState::ONLINE);
    publish_factsheet();   // VDA5050 §9.4 — publish once on connect, retained
    flush_state();
  } else {
    RCLCPP_WARN(get_logger(), "MQTT disconnected");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Executor hand-off
// ─────────────────────────────────────────────────────────────────────────────

// Queue work (work) from any thread; on_event_tick() runs it on the executor thread.
void VDA5050Node::post(std::function<void()> work)
{
  std::lock_guard<std::mutex> lock(inbound_mutex_);
  inbound_.push_back(std::move(work));
}

// Run the queued work in arrival order, then publish the state once if anything changed it.
void VDA5050Node::on_event_tick()
{
  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(inbound_mutex_);
    batch.swap(inbound_);
  }
  for (auto& work : batch)
  {
    work();
  }
  drive();

  if (state_dirty_)
  {
    flush_state();
  }
  log_mode_change();
}

// ─────────────────────────────────────────────────────────────────────────────
// ROS2 interface setup
// ─────────────────────────────────────────────────────────────────────────────

// Create ROS2 publishers/subscribers for robot interfaces (order, actions, agv state, navigation feedback); set up timers.
void VDA5050Node::setup_ros_interfaces() {
  // ── Interfaces to robot ──────────────────────────────────────────────────
  step_client_ = rclcpp_action::create_client<NavigateToNode>(this, "~/navigate_to_node");
  action_execute_pub_ = create_publisher<vda5050_msgs::msg::Action>("~/action_execute", rclcpp::QoS(10));
  action_command_pub_ = create_publisher<vda5050_msgs::msg::ActionCommand>("~/action_command", rclcpp::QoS(10));

  // ── Local status for on-robot tools ──────────────────────────────────────
  driving_pub_ = create_publisher<std_msgs::msg::Bool>("~/driving", rclcpp::QoS(1).transient_local());
  paused_pub_ = create_publisher<std_msgs::msg::Bool>("~/paused", rclcpp::QoS(1).transient_local());
  node_reached_pub_ = create_publisher<vda5050_msgs::msg::NodeState>("~/node_reached", rclcpp::QoS(10));
  error_pub_ = create_publisher<vda5050_msgs::msg::Error>("~/error", rclcpp::QoS(10));
  std_msgs::msg::Bool initial;
  driving_pub_->publish(initial);
  paused_pub_->publish(initial);

  // ── Subscribers from robot ───────────────────────────────────────────────
  using std::placeholders::_1;

  agv_pos_sub_ = create_subscription<vda5050_msgs::msg::AgvPosition>( "~/agv_position", rclcpp::QoS(10), std::bind(&VDA5050Node::on_agv_position, this, _1));

  velocity_sub_ = create_subscription<vda5050_msgs::msg::Velocity>( "~/velocity", rclcpp::QoS(10), std::bind(&VDA5050Node::on_velocity, this, _1));

  battery_sub_ = create_subscription<vda5050_msgs::msg::BatteryState>( "~/battery_state", rclcpp::QoS(10), std::bind(&VDA5050Node::on_battery_state, this, _1));

  // transient_local: the latest driver status and operating mode arrive on (re)start.
  // A finite requested lease makes every DDS vendor track the driver's liveliness; the driver's offered lease must not exceed it.
  auto driver_status_qos = rclcpp::QoS(1).transient_local();
  if (driver_status_max_lease_ > 0.0) 
  {
    driver_status_qos.liveliness(rclcpp::LivelinessPolicy::Automatic)
      .liveliness_lease_duration(rclcpp::Duration::from_seconds(driver_status_max_lease_));
  }
  rclcpp::SubscriptionOptions driver_status_options;
  driver_status_options.event_callbacks.liveliness_callback = [this](rclcpp::QOSLivelinessChangedInfo& info) 
  {
    on_driver_liveliness(info);
  };
  driver_status_options.event_callbacks.incompatible_qos_callback = [this](rclcpp::QOSRequestedIncompatibleQoSInfo&) 
  {
    RCLCPP_ERROR(get_logger(), "~/driver_status QoS incompatible: the driver's liveliness lease must not exceed "
                 "vda5050.driver_status_max_lease (%.1f s)", driver_status_max_lease_);
  };
  driver_status_sub_ = create_subscription<vda5050_msgs::msg::DriverStatus>("~/driver_status", driver_status_qos,
    std::bind(&VDA5050Node::on_driver_status, this, _1), driver_status_options);

  action_state_sub_ = create_subscription<vda5050_msgs::msg::ActionState>("~/action_state_feedback", rclcpp::QoS(10),
    std::bind(&VDA5050Node::on_action_state_feedback, this, _1));

  error_sub_ = create_subscription<vda5050_msgs::msg::Error>( "~/error", rclcpp::QoS(10),std::bind(&VDA5050Node::on_errors, this, _1));

  safety_sub_ = create_subscription<vda5050_msgs::msg::SafetyState>("~/safety_state", rclcpp::QoS(10),std::bind(&VDA5050Node::on_safety_state, this, _1));

  op_mode_sub_ = create_subscription<std_msgs::msg::String>("~/operating_mode", rclcpp::QoS(1).transient_local(), std::bind(&VDA5050Node::on_operating_mode, this, _1));

  load_sub_ = create_subscription<vda5050_msgs::msg::Load>( "~/load", rclcpp::QoS(10), std::bind(&VDA5050Node::on_load, this, _1));

  distance_since_last_node_sub_ = create_subscription<std_msgs::msg::Float64>("~/distance_since_last_node", rclcpp::QoS(10), std::bind(&VDA5050Node::on_distance_since_last_node, this, _1));

  // ── Timers ───────────────────────────────────────────────────────────────
  event_timer_ = create_wall_timer(
    std::chrono::duration<double>(event_loop_period_), [this]()
    {
      on_event_tick();
    });

  state_timer_ = create_wall_timer(
    std::chrono::duration<double>(state_publish_interval_), [this]()
    {
      flush_state();
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
std::string VDA5050Node::make_topic(const std::string& suffix) const 
{
  // uagv/v2/manufacturer/serialNumber/topic
  return interface_name_ + "/v2/" + manufacturer_ + "/" + serial_number_ + "/" + suffix;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT → ROS2  (inbound from Master Control)
// ─────────────────────────────────────────────────────────────────────────────

// Parse Order (msg) from MQTT: new order or update; the route executor drives the accepted route.
void VDA5050Node::on_order_message(const MqttMessage& msg) {
  vda5050::Order order;
  try {
    auto j = nlohmann::json::parse(msg.payload);
    order  = j.get<vda5050::Order>();
  } catch (const std::exception& e) 
  {
    RCLCPP_ERROR(get_logger(), "Failed to parse Order JSON: %s", e.what());
    report_validation_error("order", msg.payload, e.what());
    return;
  }

  RCLCPP_INFO(get_logger(), "Order received: id=%s updateId=%u",order.order_id.c_str(), order.order_update_id);

  OrderAcceptResult result;
  const bool replacing_active_order =
    order.order_id != order_manager_->current_order_id() &&
    order_manager_->has_active_order();
  if (replacing_active_order && action_manager_->has_active_order_actions())
  {
    result = OrderAcceptResult::rejected("orderError", "New order cannot replace the active order while node/edge actions are still active");
  } else 
  {
    result = order_manager_->process_order(order);
  }

  if (result.duplicate) 
  {
    RCLCPP_INFO(get_logger(), "Order %s updateId=%u is already on the vehicle -- ignored", order.order_id.c_str(), order.order_update_id);
    publish_state();
    return;
  }

  if (!result.accepted) {
    RCLCPP_WARN(get_logger(), "Order rejected (%s): %s",
                result.error_type.c_str(), result.rejection_reason.c_str());
    vda5050::Error err;
    err.error_type        = result.error_type.empty() ? "orderError" : result.error_type;
    err.error_level       = vda5050::ErrorLevel::WARNING;
    err.error_description = result.rejection_reason;
    err.error_references  = {{"orderId",       order.order_id},
                             {"orderUpdateId", std::to_string(order.order_update_id)}};
    replace_adapter_error(err);
    publish_state();
    return;
  }

  clear_errors_by_type("orderError");
  clear_errors_by_type("orderUpdateError");
  clear_errors_by_type("validationError");
  // A freshly accepted order clears any lingering navigationError/noOrderToCancel from a prior order, since neither has another natural expiry point.
  clear_errors_by_type("navigationError");
  clear_errors_by_type("noOrderToCancel");

  // A new accepted order resolves any cancelOrder still pending, since a cancel immediately
  // followed by a replacement order would otherwise never see the !driving && !order_active state it's waiting for.
  const auto superseded_cancel = state_machine_->take_pending_cancel();
  if (!superseded_cancel.empty())
   {
    action_manager_->set_action_finished(superseded_cancel, "Order cancelled (superseded by new order)");
    sync_action_blocking();
    RCLCPP_INFO(get_logger(), "Resolved pending cancelOrder %s superseded by new order %s", superseded_cancel.c_str(), order.order_id.c_str());
  }

  drive();
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
    report_validation_error("instantActions", msg.payload, e.what());
    return;
  }

  RCLCPP_INFO(get_logger(), "InstantActions received: count=%zu", ia.actions.size());

  clear_validation_errors("instantActions");
  action_manager_->process_instant_actions(ia);
  sync_action_blocking();

  publish_state();
}

// Process instant action (action): return true if handled here, false to forward it to the driver.
bool VDA5050Node::handle_instant_action(const vda5050::Action& action) {
  const auto& type = action.action_type;

  if (type == "cancelOrder") {
    const auto order_id = find_action_parameter(action, "orderId");
    const auto current_order_id = order_manager_->current_order_id();
    const bool has_active_order = order_manager_->has_active_order();
    const bool matches_active_order = order_id.empty() || order_id == current_order_id;

    if (!has_active_order || !matches_active_order)
    {
      vda5050::Error err;
      err.error_type = "noOrderToCancel";
      err.error_level = vda5050::ErrorLevel::WARNING;
      err.error_description = has_active_order ? "cancelOrder did not match the active order" : "cancelOrder received while no order is active";
      err.error_references = {{"actionId", action.action_id}};
      if (!order_id.empty())
      {
        err.error_references.push_back({"orderId", order_id});
      }
      replace_adapter_error(err);
      action_manager_->set_action_failed(action.action_id,
      has_active_order ? "No matching active order to cancel" : "No active order to cancel");
      publish_state();
      return true;
    }

    const auto replaced_cancel = state_machine_->request_cancel(action.action_id);
    if (!replaced_cancel.empty()) {
      action_manager_->set_action_finished(replaced_cancel, "Superseded by newer cancelOrder");
    }
    clear_errors_by_type("noOrderToCancel");
    action_manager_->cancel_all(action.action_id);
    order_manager_->cancel_order(order_id);
    action_manager_->set_action_running(action.action_id);
    drive();
    maybe_complete_pending_control_actions();
    sync_action_blocking();
    publish_state();
    RCLCPP_INFO(get_logger(), "cancelOrder: %s", current_order_id.c_str());
    return true;
  }

  if (type == "startPause")
  {
    const auto replaced_pending = state_machine_->request_pause(action.action_id);
    if (!replaced_pending.empty()) {
      action_manager_->set_action_finished(replaced_pending, "Superseded by newer startPause");
    }
    action_manager_->pause_all(action.action_id);
    action_manager_->set_action_running(action.action_id);
    pause_requested_ = true;
    drive();
    maybe_complete_pending_control_actions();
    sync_action_blocking();
    publish_state();
    RCLCPP_INFO(get_logger(), "startPause");
    return true;
  }

  if (type == "stopPause")
  {
    const auto replaced_pending = state_machine_->request_resume(action.action_id);
    if (!replaced_pending.empty()) {
      action_manager_->set_action_finished(replaced_pending, "Superseded by newer stopPause");
    }
    action_manager_->resume_all(action.action_id);
    action_manager_->set_action_running(action.action_id);
    pause_requested_ = false;
    drive();
    maybe_complete_pending_control_actions();
    sync_action_blocking();
    publish_state();
    RCLCPP_INFO(get_logger(), "stopPause");
    return true;
  }

  if (type == "stateRequest") {
    action_manager_->set_action_running(action.action_id);
    action_manager_->set_action_finished(action.action_id, "State published");
    sync_action_blocking();
    publish_state();
    return true;
  }

  if (type == "factsheetRequest") {
    action_manager_->set_action_running(action.action_id);
    publish_factsheet();
    action_manager_->set_action_finished(action.action_id, "Factsheet published");
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

// Mark the state as changed; on_event_tick() publishes it once per tick.
void VDA5050Node::publish_state() {
  state_dirty_ = true;
}

// Build state snapshot from managers and publish to MQTT state topic; no-op if not connected.
void VDA5050Node::flush_state() {
  state_dirty_ = false;
  if (!mqtt_client_ || !mqtt_client_->is_connected()) return;

  nlohmann::json j = build_state_snapshot();
  mqtt_client_->publish(make_topic("state"), j.dump(), 0, false);
}

// Publish connection state (conn_state) to MQTT connection topic with QoS 1 retained.
void VDA5050Node::publish_connection(vda5050::ConnectionState conn_state) {
  if (!mqtt_client_) return;

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
  if (agv_position_set_) viz.agv_position = agv_position_;
  if (velocity_set_)     viz.velocity     = velocity_;
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

  // retained=true so Master Control gets it immediately on (re-)subscribe QoS 0 per VDA5050 §6.2 (factsheet is informational, not safety-critical like connection)
  mqtt_client_->publish(make_topic("factsheet"), j.dump(), 0, true);
  RCLCPP_INFO(get_logger(), "Factsheet published");
}

// Build VDA5050 Factsheet from ROS2 parameters (type_specification, physical_parameters, protocol_features).
vda5050::Factsheet VDA5050Node::build_factsheet_from_params() const {
  vda5050::Factsheet fs;

  // ── typeSpecification ──────────────────────────────────────────────────────
  fs.type_specification.series_name        = get_parameter("factsheet.type_specification.series_name").as_string();

  fs.type_specification.series_description = get_parameter("factsheet.type_specification.series_description").as_string();

  fs.type_specification.agv_kinematic = get_parameter("factsheet.type_specification.agv_kinematic").as_string();

  fs.type_specification.agv_class     = get_parameter("factsheet.type_specification.agv_class").as_string();

  fs.type_specification.max_load_mass = get_parameter("factsheet.type_specification.max_load_mass").as_double();
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
    {"startPause",       "Pause AGV movement and all running actions",            {"NONE"}},
    {"stopPause",        "Resume AGV movement and paused actions",                {"NONE"}},
    {"cancelOrder",      "Cancel the current order and clear the route",          {"NONE"}},
    {"stateRequest",     "Trigger an immediate state message publication",        {"NONE"}},
    {"factsheetRequest", "Trigger an immediate factsheet publication",            {"NONE"}},
    {"initPosition",     "Initialize the AGV position on the map",               {"NONE"}},
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
  agv_position_     = internal_from_ros(*msg);
  agv_position_set_ = true;

  // Throttled to position_publish_min_interval_ so a fast ~agv_position publisher can't flood the broker.
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration<double>(now - last_position_publish_).count();
  if (elapsed >= position_publish_min_interval_) {
    last_position_publish_ = now;
    publish_state();
  }
}

// Receive AGV velocity (msg): store for visualization publication.
void VDA5050Node::on_velocity(
  const vda5050_msgs::msg::Velocity::SharedPtr msg)
{
  velocity_     = internal_from_ros(*msg);
  velocity_set_ = true;
}

// Receive battery state (msg): store for state publication.
void VDA5050Node::on_battery_state(
  const vda5050_msgs::msg::BatteryState::SharedPtr msg)
{
  battery_state_ = internal_from_ros(*msg);
}

// Driver status (msg): a changed session drops the lost step goal and resends it; driving goes to the state machine.
void VDA5050Node::on_driver_status(const vda5050_msgs::msg::DriverStatus::SharedPtr msg)
{
  if (!driver_session_.empty() && msg->session_id != driver_session_) {
    const bool lost = step_ && step_->session != msg->session_id;
    RCLCPP_WARN(get_logger(), "Driver restarted (session %s -> %s)%s", driver_session_.c_str(), msg->session_id.c_str(), lost ? " -- resending the active step" : "");
    if (lost) {
      step_.reset();
      sync_order_activity();
    }
    stale_goals_cleared_ = true;
  }
  driver_session_ = msg->session_id;

  const bool changed = set_driver_driving(msg->driving);
  drive();

  if (maybe_complete_pending_control_actions() || changed) {
    publish_state();
  }
}

void VDA5050Node::on_driver_liveliness(const rclcpp::QOSLivelinessChangedInfo& info)
{
  if (info.alive_count > 0) {
    if (!driver_lost_) return;
    driver_lost_ = false;
    RCLCPP_WARN(get_logger(), "Driver back");
    if (!stale_goals_cleared_) {
      stale_goals_cancel_sent_ = false;
    }
    clear_errors_by_type(kDriverLostError);
    publish_state();
    return;
  }
  if (driver_lost_ || info.alive_count_change >= 0) return;

  driver_lost_ = true;
  RCLCPP_ERROR(get_logger(), "Driver lost: driver_status liveliness expired%s",
               step_ ? " -- step in flight dropped" : "");
  step_.reset();
  set_driver_driving(false);

  vda5050::Error err;
  err.error_type        = kDriverLostError;
  err.error_level       = vda5050::ErrorLevel::FATAL;
  err.error_description = "Robot driver lost: no driver_status within its liveliness lease";
  replace_adapter_error(err);

  sync_order_activity();
  drive();
  maybe_complete_pending_control_actions();
  publish_state();
}

bool VDA5050Node::set_driver_driving(bool driving)
{
  state_machine_->on_driver_driving_changed(driving);
  if (driving == driving_) return false;
  driving_ = driving;
  std_msgs::msg::Bool out;
  out.data = driving;
  driving_pub_->publish(out);
  return true;
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

// Receive error from driver (msg): upsert and publish the state at once (VDA5050 §7.3). A
// navigationError naming an order that is no longer active is stale and ignored (also this node's own copy).
void VDA5050Node::on_errors(const vda5050_msgs::msg::Error::SharedPtr msg) {
  const auto error = internal_from_ros(*msg);
  if (error.error_type == "navigationError") {
    const auto order_id = error_reference(error, "orderId");
    if (!order_id.empty() &&
        (!order_manager_->has_active_order() || order_id != order_manager_->current_order_id())) {
      return;
    }
  }
  upsert_driver_error(error);
  flush_state();
}

// Receive safety state (msg): store for state publication.
void VDA5050Node::on_safety_state(
  const vda5050_msgs::msg::SafetyState::SharedPtr msg)
{
  safety_state_ = internal_from_ros(*msg);
}

// Receive operating mode (msg): store it; an unknown name keeps the current mode.
void VDA5050Node::on_operating_mode(
  const std_msgs::msg::String::SharedPtr msg)
{
  const auto mode = parse_operating_mode(msg->data);
  if (!mode.has_value()) {
    RCLCPP_WARN(get_logger(), "Unknown operating mode '%s' from the driver -- ignored", msg->data.c_str());
    return;
  }
  if (*mode != operating_mode_) {
    operating_mode_ = *mode;
    publish_state();
  }
}

// Receive load state (msg): store for state publication.
void VDA5050Node::on_load(const vda5050_msgs::msg::Load::SharedPtr msg)
{
  auto it = std::find_if(loads_.begin(), loads_.end(),[&](const vda5050::Load& l)
    {
      return l.load_id == msg->load_id;
    });
  if (it != loads_.end()) *it = internal_from_ros(*msg);
  else loads_.push_back(internal_from_ros(*msg));
}

// ─────────────────────────────────────────────────────────────────────────────
// Route execution
// ─────────────────────────────────────────────────────────────────────────────

// Keep one step goal toward the next node while driving is allowed; cancel it otherwise.
void VDA5050Node::drive()
{
  clear_stale_goals();
  std::optional<RouteStep> next;
  if (may_drive()) {
    next = order_manager_->next_step();
  }

  if (!next) {
    if (step_ && !step_->cancel_requested) {
      cancel_step();
    }
  } else if (!step_ || step_->cancel_requested || !same_step(step_->step, *next)) {
    if (driver_ready()) {
      send_step(*next);
    }
  }
  update_paused();
}

void VDA5050Node::clear_stale_goals()
{
  if (stale_goals_cleared_ || stale_goals_cancel_sent_ || driver_lost_ ||
      !step_client_->action_server_is_ready()) {
    return;
  }
  stale_goals_cancel_sent_ = true;
  using CancelResponse = rclcpp_action::Client<NavigateToNode>::CancelResponse;
  step_client_->async_cancel_all_goals([this](const CancelResponse::SharedPtr response) {
    stale_goals_cleared_ = true;
    if (response && !response->goals_canceling.empty()) {
      RCLCPP_WARN(get_logger(), "Cancelled %zu step goal(s) left on the driver by a previous adapter process",
                  response->goals_canceling.size());
    }
  });
}

bool VDA5050Node::driver_ready() const
{
  return stale_goals_cleared_ && !driver_lost_ && step_client_->action_server_is_ready();
}

bool VDA5050Node::may_drive() const
{
  if (pause_requested_) return false;
  return !strict_mode_ || (!action_manager_->is_hard_blocked() && !action_manager_->is_soft_blocked());
}

// Send `step` as a new goal; the driver preempts the goal in flight, whose result is then ignored.
void VDA5050Node::send_step(const RouteStep& step)
{
  NavigateToNode::Goal goal;
  goal.order_id        = step.order_id;
  goal.order_update_id = step.order_update_id;
  goal.node            = ros_from_internal(step.node);
  if (step.incoming_edge) {
    goal.incoming_edge     = ros_from_internal(*step.incoming_edge);
    goal.incoming_edge_set = true;
  }

  const uint64_t token = ++step_token_;
  step_ = InFlightStep{token, step, nullptr, false, driver_session_};
  RCLCPP_INFO(get_logger(), "Step: order=%s node=%s (seq=%u)", step.order_id.c_str(),
              step.node.node_id.c_str(), step.node.sequence_id);

  rclcpp_action::Client<NavigateToNode>::SendGoalOptions options;
  options.goal_response_callback = [this, token](const StepGoalHandle::SharedPtr& handle) 
  {
    on_step_response(token, handle);
  };
  options.feedback_callback = [this, token](StepGoalHandle::SharedPtr,const std::shared_ptr<const NavigateToNode::Feedback> feedback) 
  {
    if (feedback->edge_entered) on_step_edge_entered(token);
  };
  options.result_callback = [this, token](const StepGoalHandle::WrappedResult& result) 
  {
    on_step_result(token, result);
  };
  step_client_->async_send_goal(goal, options);
  sync_order_activity();
}

// Cancel the step in flight; it stays in flight until the driver reports its result.
void VDA5050Node::cancel_step()
{
  step_->cancel_requested = true;
  if (step_->handle) 
  {
    step_client_->async_cancel_goal(step_->handle);
  }
}

void VDA5050Node::on_step_response(uint64_t token, const StepGoalHandle::SharedPtr& handle)
{
  if (!step_ || step_->token != token) return;
  if (!handle) 
  {
    auto rejected = std::move(step_->step);
    step_.reset();
    drop_order(rejected, "Driver rejected the NavigateToNode goal", true);
    drive();
    return;
  }
  step_->handle = handle;
  if (step_->cancel_requested) 
  {
    step_client_->async_cancel_goal(handle);
  }
}

void VDA5050Node::on_step_edge_entered(uint64_t token)
{
  if (!step_ || step_->token != token) return;
  enter_step_edge(step_->step);
  sync_action_blocking();
  publish_state();
}

// Apply the result of the step in flight: REACHED advances the route, FAILED/DROPPED drop the order.
void VDA5050Node::on_step_result(uint64_t token, const StepGoalHandle::WrappedResult& result)
{
  if (!step_ || step_->token != token) return;
  auto finished = std::move(*step_);
  step_.reset();

  // SUCCEEDED is the only REACHED; ABORTED carries FAILED / DROPPED / PREEMPTED; anything else made no progress.
  uint8_t outcome = NavigateToNode::Result::CANCELED;
  if (result.code == rclcpp_action::ResultCode::SUCCEEDED) 
  {
    outcome = NavigateToNode::Result::REACHED;
  } else if (result.code == rclcpp_action::ResultCode::ABORTED && result.result && result.result->outcome != NavigateToNode::Result::REACHED) {
    outcome = result.result->outcome;
  }
  const std::string description = result.result ? result.result->description : "";
  switch (outcome) 
  {
    case NavigateToNode::Result::REACHED:
      complete_step(finished.step, result.result ? result.result->distance_driven : 0.0);
      break;
    case NavigateToNode::Result::FAILED:
    case NavigateToNode::Result::DROPPED:
      if (!finished.cancel_requested)
       {
        drop_order(finished.step, description, outcome == NavigateToNode::Result::FAILED);
      }
      break;
    default:
      break;
  }

  sync_order_activity();
  sync_action_blocking();
  maybe_complete_pending_control_actions();
  publish_state();
  drive();
}

void VDA5050Node::enter_step_edge(RouteStep& step)
{
  if (!step.incoming_edge || step.edge_entered) return;
  step.edge_entered = true;
  const auto& edge = *step.incoming_edge;
  if (order_manager_->edge_entered(edge.edge_id, edge.sequence_id)) {
    RCLCPP_INFO(get_logger(), "Edge entered: %s (seq=%u)", edge.edge_id.c_str(), edge.sequence_id);
    action_manager_->on_edge_entered(edge.edge_id, edge.sequence_id);
  }
}

void VDA5050Node::complete_step(RouteStep& step, double distance_driven)
{
  if (order_manager_->current_order_id() != step.order_id) return;

  enter_step_edge(step);
  if (step.incoming_edge) 
  {
    const auto& edge = *step.incoming_edge;
    if (order_manager_->edge_completed(edge.edge_id, edge.sequence_id)) {
      RCLCPP_INFO(get_logger(), "Edge completed: %s (seq=%u)", edge.edge_id.c_str(), edge.sequence_id);
      action_manager_->on_edge_left(edge.edge_id, edge.sequence_id);
    }
  }

  const auto& node = step.node;
  if (!order_manager_->node_reached({node.node_id, node.sequence_id, distance_driven})) {
    return;
  }
  RCLCPP_INFO(get_logger(), "Node reached: %s (seq=%u)", node.node_id.c_str(), node.sequence_id);
  action_manager_->on_node_reached(node.node_id, node.sequence_id);

  vda5050_msgs::msg::NodeState reached;
  reached.node_id           = node.node_id;
  reached.sequence_id       = node.sequence_id;
  reached.node_description  = node.node_description;
  reached.released          = node.released;
  reached.distance_driven   = distance_driven;
  if (node.node_position) {
    reached.node_position     = ros_from_internal(*node.node_position);
    reached.node_position_set = true;
  }
  node_reached_pub_->publish(reached);
}

void VDA5050Node::drop_order(const RouteStep& step, const std::string& reason, bool failed)
{
  if (!order_manager_->has_active_order() || order_manager_->current_order_id() != step.order_id)
   {
    return;
  }
  RCLCPP_WARN(get_logger(), "Order '%s' %s by the driver at node %s: %s", step.order_id.c_str(),
              failed ? "failed" : "dropped", step.node.node_id.c_str(), reason.c_str());

  if (failed) {
    vda5050::Error err;
    err.error_type        = "navigationError";
    err.error_level       = vda5050::ErrorLevel::FATAL;
    err.error_description = reason;
    err.error_references  = {{"orderId", step.order_id}, {"nodeId", step.node.node_id}};
    upsert_driver_error(err);
    flush_state();
    error_pub_->publish(ros_from_internal(err));
  }
  action_manager_->cancel_all("");
  order_manager_->cancel_order(step.order_id);
  clear_errors_by_type("navigationError");
  sync_action_blocking();
  publish_state();
}

void VDA5050Node::update_paused()
{
  const bool paused = pause_requested_ && !step_;
  if (paused == paused_) return;
  paused_ = paused;
  std_msgs::msg::Bool out;
  out.data = paused;
  paused_pub_->publish(out);
  state_machine_->on_driver_paused_changed(paused);
  maybe_complete_pending_control_actions();
  publish_state();
}

// Live distance-since-last-node reading (msg) -- just stored; the periodic state_timer_ publish reports it, so no publish_state() here.
void VDA5050Node::on_distance_since_last_node(const std_msgs::msg::Float64::SharedPtr msg)
{
  order_manager_->set_distance_since_last_node(msg->data);
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderManager callbacks
// ─────────────────────────────────────────────────────────────────────────────

// OrderManager callback: order was accepted. Reset action state for new order, publish to robot.
void VDA5050Node::on_order_accepted(const std::string& order_id,
                                      uint32_t order_update_id,
                                      const std::vector<vda5050::Node>& remaining_nodes,
                                      const std::vector<vda5050::Edge>& remaining_edges)
{
  RCLCPP_INFO(get_logger(), "Order accepted: %s (updateId=%u, nodes=%zu, edges=%zu)",
              order_id.c_str(), order_update_id, remaining_nodes.size(), remaining_edges.size());

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
  RCLCPP_INFO(get_logger(), "Execute action: type=%s id=%s", action.action_type.c_str(), action.action_id.c_str());

  if (handle_instant_action(action)) return;

  action_execute_pub_->publish(ros_from_internal(action));
  publish_state();
}

// ActionManager callback: ask the driver to pause one action (action_id).
void VDA5050Node::on_action_pause(const std::string& action_id)
{
  send_action_command(vda5050_msgs::msg::ActionCommand::PAUSE, action_id);
}

// ActionManager callback: ask the driver to resume one action (action_id).
void VDA5050Node::on_action_resume(const std::string& action_id)
{
  send_action_command(vda5050_msgs::msg::ActionCommand::RESUME, action_id);
}

// ActionManager callback: ask the driver to cancel one action (action_id).
void VDA5050Node::on_action_cancel(const std::string& action_id)
{
  send_action_command(vda5050_msgs::msg::ActionCommand::CANCEL, action_id);
  publish_state();
}

// Publish (command, action_id) on ~/action_command.
void VDA5050Node::send_action_command(uint8_t command, const std::string& action_id)
{
  vda5050_msgs::msg::ActionCommand msg;
  msg.command   = command;
  msg.action_id = action_id;
  RCLCPP_INFO(get_logger(), "Action command %u: %s", command, action_id.c_str());
  action_command_pub_->publish(msg);
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

// ─────────────────────────────────────────────────────────────────────────────
// Errors
// ─────────────────────────────────────────────────────────────────────────────

// Replace adapter error (error) of same type; recompute fatal error state for state machine.
void VDA5050Node::replace_adapter_error(const vda5050::Error& error)
{
  errors_.erase(
    std::remove_if(errors_.begin(), errors_.end(), [&](const vda5050::Error& existing)
    {
      return existing.error_type == error.error_type;
    }),
    errors_.end());
  errors_.push_back(error);
  update_fatal_state();
}

// Upsert driver error (error) into list (merge by identity, skip adapter-owned error types); recompute fatal status.
void VDA5050Node::upsert_driver_error(const vda5050::Error& error)
{
  const auto identity = error_identity(error);
  auto it = std::find_if(errors_.begin(), errors_.end(),
                         [&](const vda5050::Error& existing)
                         {
                           return !is_adapter_error_type(existing.error_type) && error_identity(existing) == identity;
                         });
  if (it != errors_.end())
  {
    *it = error;
  }
  else
  {
    errors_.push_back(error);
  }
  update_fatal_state();
}

// Clear all errors of type (error_type) from list; recompute fatal error state for state machine.
void VDA5050Node::clear_errors_by_type(const std::string& error_type)
{
  errors_.erase(
    std::remove_if(errors_.begin(), errors_.end(),
                   [&](const vda5050::Error& error) {
                     return error.error_type == error_type;
                   }),
    errors_.end());
  update_fatal_state();
}

// Clear the validationErrors raised for messages on topic (topic).
void VDA5050Node::clear_validation_errors(const std::string& topic)
{
  errors_.erase(
    std::remove_if(errors_.begin(), errors_.end(),
                   [&](const vda5050::Error& error) 
                   {
                     return error.error_type == "validationError" && error_reference(error, kTopicReference) == topic;
                   }),
    errors_.end());
  update_fatal_state();
}

// Report an unparsable message (payload) on topic as validationError (VDA5050 §6.6.4.1).
void VDA5050Node::report_validation_error(const std::string& topic, const std::string& payload, const std::string& reason)
{
  vda5050::Error err;
  err.error_type        = "validationError";
  err.error_level       = vda5050::ErrorLevel::WARNING;
  err.error_description = reason;
  err.error_references  = {{kTopicReference, topic}};

  // Name the order when the payload is valid JSON with an orderId.
  const auto j = nlohmann::json::parse(payload, nullptr, false);
  if (j.is_object() && j.contains("orderId") && j["orderId"].is_string())
  {
    err.error_references.push_back({"orderId", j["orderId"].get<std::string>()});
  }

  clear_validation_errors(topic);
  errors_.push_back(err);
  update_fatal_state();
  publish_state();
}

// Tell the state machine whether any FATAL error is present.
void VDA5050Node::update_fatal_state()
{
  const bool has_fatal_error = std::any_of(errors_.begin(), errors_.end(), [](const vda5050::Error& entry) {
    return entry.error_level == vda5050::ErrorLevel::FATAL;
  });
  state_machine_->on_fatal_error_changed(has_fatal_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// State machine sync
// ─────────────────────────────────────────────────────────────────────────────

// Order activity for the state machine: an active order or a step goal still in flight.
void VDA5050Node::sync_order_activity()
{
  state_machine_->on_order_state_changed(order_manager_->has_active_order() || step_.has_value());
}

// Sync action blocking state: notify state machine if any HARD/SOFT action is blocking.
void VDA5050Node::sync_action_blocking()
{
  const bool action_blocked =  action_manager_->is_hard_blocked() || action_manager_->is_soft_blocked();
  state_machine_->on_action_blocking_changed(action_blocked);
}

// Log adapter mode transitions.
void VDA5050Node::log_mode_change()
{
  const auto mode = state_machine_->mode();
  if (mode == logged_mode_) return;
  RCLCPP_INFO(get_logger(), "Adapter mode: %s -> %s", AdapterStateMachine::to_string(logged_mode_), AdapterStateMachine::to_string(mode));
  logged_mode_ = mode;
}

// ─────────────────────────────────────────────────────────────────────────────
// State snapshot
// ─────────────────────────────────────────────────────────────────────────────

// Build complete VDA5050 State message from order, action, and robot state snapshots.
vda5050::State VDA5050Node::build_state_snapshot() const
{
  vda5050::State s;
  s.header = make_header("state");

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

  if (agv_position_set_) s.agv_position = agv_position_;
  if (velocity_set_)     s.velocity     = velocity_;
  s.battery_state         = battery_state_;
  s.safety_state          = safety_state_;
  s.loads                 = loads_;
  s.errors                = errors_;
  s.operating_mode        = operating_mode_;

  return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Header factory
// ─────────────────────────────────────────────────────────────────────────────

// Build VDA5050 header for message topic (topic): increment per-topic header_id, set timestamp/version/manufacturer/serial.
vda5050::Header VDA5050Node::make_header(const std::string& topic) const
 {
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
