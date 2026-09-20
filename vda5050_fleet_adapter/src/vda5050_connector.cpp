#include "vda5050_fleet_adapter/vda5050_connector.hpp"

#include <unistd.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter {

namespace proto = protocol;

namespace {

constexpr std::chrono::seconds kFactsheetFirstWait{5};
constexpr std::chrono::seconds kFactsheetRetryWait{20};
constexpr int kFactsheetRequestAttempts = 3;

}  // namespace

Vda5050Connector::Vda5050Connector(
  rclcpp::Logger logger, std::string broker_url, std::string interface_name,
  std::optional<std::string> username, std::optional<std::string> password)
  : _logger(std::move(logger)), _interface_name(std::move(interface_name))
{

  _client = std::make_shared<mqtt::async_client>(
    broker_url, "rmf_vda5050_adapter_" + std::to_string(::getpid()));
  _client->set_callback(*this);

  _conn_opts.set_clean_session(true);
  _conn_opts.set_keep_alive_interval(60);
  _conn_opts.set_automatic_reconnect(true);
  if (username.has_value())
    _conn_opts.set_user_name(*username);
  if (password.has_value())
    _conn_opts.set_password(*password);
}

Vda5050Connector::~Vda5050Connector()
{
  shutdown();
}

void Vda5050Connector::start()
{
  try {
    _client->connect(_conn_opts)->wait();
    RCLCPP_INFO(_logger, "[VDA5050] MQTT connected");
  } catch (const mqtt::exception& e) {
    RCLCPP_ERROR(_logger, "[VDA5050] MQTT connect failed: %s", e.what());
  }
}

void Vda5050Connector::shutdown()
{
  if (_shutdown.exchange(true))
    return; 
  if (_client && _client->is_connected()) 
  {
    try {
      _client->disconnect()->wait_for(std::chrono::seconds(1));
    } 
    catch (const mqtt::exception&) 
    {
    }
  }
}

void Vda5050Connector::add_robot(const std::string& name,
                                 const std::string& manufacturer,
                                 const std::string& serial,
                                 const Transform& transform)
{
  auto ctx = std::make_unique<RobotContext>();
  ctx->name = name;
  ctx->manufacturer = manufacturer;
  ctx->serial = serial;
  ctx->interface_name = _interface_name;
  ctx->transform = transform;

  RobotContext* ctx_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    ctx_ptr = ctx.get();
    _robots[name] = std::move(ctx);
  }

  subscribe_robot(*ctx_ptr);
  RCLCPP_INFO(_logger, "[VDA5050] robot '%s' -> %s/%s", name.c_str(), manufacturer.c_str(), serial.c_str());

  request_state(name);
}

void Vda5050Connector::subscribe_robot(const RobotContext& ctx)
{
  if (!_client->is_connected())
    return;
  for (const char* leaf :
       {proto::TOPIC_STATE, proto::TOPIC_CONNECTION, proto::TOPIC_VISUALIZATION,
        proto::TOPIC_FACTSHEET})
  {
    _client->subscribe(
      proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, leaf), 1);
  }
}

// ─── mqtt::callback ────────────────────────────────────────────────────────────

void Vda5050Connector::connected(const std::string&)
{
  RCLCPP_INFO(_logger, "[VDA5050] MQTT (re)connected — re-subscribing");
  std::vector<RobotContext*> ctxs;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& [_, ctx] : _robots)
      ctxs.push_back(ctx.get());
  }

  for (RobotContext* ctx : ctxs)
    subscribe_robot(*ctx);
}

void Vda5050Connector::connection_lost(const std::string& cause)
{
  RCLCPP_WARN(_logger, "[VDA5050] MQTT connection lost: %s", cause.c_str());
}

void Vda5050Connector::message_arrived(mqtt::const_message_ptr msg)
{
  nlohmann::json payload;
  try 
  {
    payload = nlohmann::json::parse(msg->get_payload_str());
  } 
  catch (const std::exception&) 
  {
    RCLCPP_WARN(_logger, "[VDA5050] bad payload on %s", msg->get_topic().c_str());
    return;
  }

  const std::string topic = msg->get_topic();
  std::lock_guard<std::mutex> lock(_mutex);
  RobotContext* ctx = match_robot(topic);
  if (!ctx)
    return;

  auto ends_with = [&](const char* leaf) 
  {
    const std::string s = leaf;
    return topic.size() >= s.size() &&
           topic.compare(topic.size() - s.size(), s.size(), s) == 0;
  };

  if (ends_with(proto::TOPIC_STATE)) 
  {
    ctx->last_state = proto::ParsedState(payload);
    ctx->last_state_time = std::chrono::steady_clock::now();
    if (!ctx->last_state->last_node_id.empty())
      ctx->last_node_id = ctx->last_state->last_node_id;

    // Log errors and pause state when they change.
    std::string errors_key;
    for (const auto& e : ctx->last_state->errors)
      errors_key += e.dump() + ";";
    if (ctx->last_state->paused)
      errors_key += "paused;";
    if (errors_key != ctx->last_errors_key)
    {
      if (errors_key.empty())
      {
        RCLCPP_INFO(_logger, "[VDA5050] %s: errors cleared",
                    ctx->name.c_str());
      }
      else
      {
        RCLCPP_ERROR(_logger, "[VDA5050] %s reports: %s", ctx->name.c_str(),
                     errors_key.c_str());
      }
      ctx->last_errors_key = errors_key;
    }

    const std::string safety_key = ctx->last_state->safety_state.triggered()
      ? "eStop=" + ctx->last_state->safety_state.e_stop +
        (ctx->last_state->safety_state.field_violation ? " fieldViolation" : "")
      : std::string{};
    if (safety_key != ctx->last_safety_key)
    {
      if (safety_key.empty())
      {
        RCLCPP_INFO(_logger, "[VDA5050] %s: safety state cleared", ctx->name.c_str());
      }
      else
      {
        RCLCPP_ERROR(_logger, "[VDA5050] %s safety state active: %s",
                     ctx->name.c_str(), safety_key.c_str());
      }
      ctx->last_safety_key = safety_key;
    }

    if (ctx->last_state->operating_mode != ctx->last_mode)
    {
      RCLCPP_INFO(_logger, "[VDA5050] %s operating mode: %s", ctx->name.c_str(),
                  ctx->last_state->operating_mode.c_str());
      ctx->last_mode = ctx->last_state->operating_mode;
    }
  } else if (ends_with(proto::TOPIC_FACTSHEET))
  {
    proto::ParsedFactsheet factsheet(payload);
    if (factsheet.has_content())
    {
      std::string actions;
      for (const auto& [type, info] : factsheet.agv_actions)
      {
        actions += (actions.empty() ? "" : ", ") + type;
      }
      RCLCPP_INFO(_logger, "[VDA5050] %s factsheet: series '%s', actions: %s",
                  ctx->name.c_str(), factsheet.series_name.c_str(),
                  actions.empty() ? "(none)" : actions.c_str());
      ctx->factsheet = std::move(factsheet);
      ctx->factsheet_requests = 0;
    }
  } else if (ends_with(proto::TOPIC_CONNECTION)) 
  {
    const std::string conn = payload.value("connectionState", std::string{});
    const bool online = (conn == "ONLINE");
    if (ctx->connected != online) 
    {
      if (online)
      {
        RCLCPP_INFO(_logger, "[VDA5050] %s ONLINE", ctx->name.c_str());
        ctx->factsheet_requests = 0;
        ctx->factsheet_wait_since = std::chrono::steady_clock::now();
      }
        
      else if (ctx->connected == true)
      {
        RCLCPP_WARN(_logger, "[VDA5050] %s %s", ctx->name.c_str(),
            conn.empty() ? "OFFLINE" : conn.c_str());
      }

    }
    ctx->connected = online;
  }
}

Vda5050Connector::RobotContext* Vda5050Connector::match_robot(
  const std::string& topic)
{
  for (auto& [_, ctx] : _robots) {
    const std::string needle = "/" + ctx->manufacturer + "/" + ctx->serial + "/";
    if (topic.find(needle) != std::string::npos)
      return ctx.get();
  }
  return nullptr;
}

// ─── RMF -> AGV (downlink) ──────────────────────────────────────────────────────

void Vda5050Connector::navigate(const std::string& name,
                                const std::string& dest_node_id, double x,
                                double y, double theta,
                                const std::string& map_id,
                                std::optional<double> speed_limit)
{
  std::string order_id;
  std::string base_id;
  std::string manufacturer, serial, interface_name;
  std::array<double, 3> dest{}, base{};
  int header_id = 0;
  std::optional<double> edge_speed = speed_limit;

  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end()) 
    {
      RCLCPP_ERROR(_logger, "[VDA5050] navigate: unknown robot '%s'",
                   name.c_str());
      return;
    }
    RobotContext& ctx = *it->second;

    dest = ctx.transform.to_robot(x, y, theta);
    base = dest;
    base_id = ctx.last_node_id.empty() ? (ctx.serial + "_start") : ctx.last_node_id;
    if (ctx.last_state.has_value() && ctx.last_state->has_position()) 
    {
      base = {*ctx.last_state->x, *ctx.last_state->y, *ctx.last_state->theta};
    }

    if (ctx.speed_limit.has_value())
    {
      edge_speed = edge_speed.has_value() ? std::min(*edge_speed, *ctx.speed_limit)
                                          : *ctx.speed_limit;
    }

    order_id = proto::make_uuid();
    ctx.current_order_id = order_id;
    ctx.target_node_id = dest_node_id;
    header_id = ctx.next_order_header();
    manufacturer = ctx.manufacturer;
    serial = ctx.serial;
    interface_name = ctx.interface_name;
  }


  nlohmann::json nodes = nlohmann::json::array();
  nodes.push_back(proto::make_node(base_id, 0, base[0], base[1], base[2], map_id));
  nodes.push_back(
    proto::make_node(dest_node_id, 2, dest[0], dest[1], dest[2], map_id));
  nlohmann::json edges = nlohmann::json::array();
  edges.push_back(proto::make_edge("e_" + base_id + "_" + dest_node_id, 1,
                                   base_id, dest_node_id, true, edge_speed));

  const auto order = proto::make_order(header_id, manufacturer, serial, nodes,
                                       edges, order_id, 0);

  const std::string order_topic = proto::topic(interface_name, manufacturer, serial, proto::TOPIC_ORDER);
  publish_raw(order_topic, order.dump());
  RCLCPP_INFO(_logger, "[VDA5050] %s -> order '%s' to node '%s' (%.2f, %.2f)",
              name.c_str(), order_id.c_str(), dest_node_id.c_str(), dest[0],dest[1]);
}

void Vda5050Connector::stop(const std::string& name)
{
  std::string topic;
  nlohmann::json msg;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
      return;
    RobotContext& ctx = *it->second;

    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(proto::cancel_order_action("", blocking_type_for(ctx, "cancelOrder", "HARD")));
    msg = proto::make_instant_actions(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,proto::TOPIC_INSTANT_ACTIONS);
    ctx.current_order_id.clear();
    ctx.target_node_id.clear();
  }
  publish_raw(topic, msg.dump());
  RCLCPP_INFO(_logger, "[VDA5050] %s -> cancelOrder", name.c_str());
}

std::string Vda5050Connector::execute_instant_action(
  const std::string& name, const std::string& action_type,
  const std::vector<std::pair<std::string, std::string>>& parameters)
{
  std::string topic;
  std::string action_id;
  nlohmann::json msg;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
      return {};
    RobotContext& ctx = *it->second;

    const auto verdict = proto::check_instant_action(action_type, ctx.factsheet);
    if (verdict.result == proto::ActionCheck::reject && _strict_validation)
    {
      RCLCPP_ERROR(_logger, "[VDA5050] %s: %s -- not sent", name.c_str(),
                   verdict.message.c_str());
      return {};
    }
    if (verdict.result != proto::ActionCheck::allowed)
    {
      RCLCPP_WARN(_logger, "[VDA5050] %s: %s -- sending it anyway", name.c_str(),
                  verdict.message.c_str());
    }

    const std::string blocking = blocking_type_for(ctx, action_type, "HARD");
    if (ctx.last_state.has_value())
    {
      for (const auto& conflict : proto::action_conflicts( action_type, blocking, ctx.last_state->driving,
             ctx.last_state->action_states, ctx.factsheet))
      {
        RCLCPP_WARN(_logger, "[VDA5050] %s: %s", name.c_str(), conflict.c_str());
      }
    }

    const auto action = proto::make_action(action_type, blocking, "", parameters);
    action_id = action.value("actionId", std::string{});
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(action);
    msg = proto::make_instant_actions(ctx.next_instant_actions_header(), ctx.manufacturer,  ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, proto::TOPIC_INSTANT_ACTIONS);
  }
  publish_raw(topic, msg.dump());
  return action_id;
}

void Vda5050Connector::request_state(const std::string& name)
{
  std::string topic;
  nlohmann::json msg;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
      return;
    RobotContext& ctx = *it->second;

    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(proto::make_action("stateRequest", "NONE", "", {}));
    msg = proto::make_instant_actions(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, proto::TOPIC_INSTANT_ACTIONS);
  }
  publish_raw(topic, msg.dump());
}

bool Vda5050Connector::publish_raw(const std::string& topic,
                                   const std::string& payload)
{
  if (!_client->is_connected()) {
    RCLCPP_WARN(_logger, "[VDA5050] not connected, dropping publish to %s",
                topic.c_str());
    return false;
  }
  try
  {
    auto m = mqtt::make_message(topic, payload);
    m->set_qos(1);
    _client->publish(m);
  }
  catch (const mqtt::exception& e)
  {
    RCLCPP_ERROR(_logger, "[VDA5050] publish to %s failed: %s", topic.c_str(), e.what());
    return false;
  }
  return true;
}

std::string Vda5050Connector::blocking_type_for(const RobotContext& ctx, const std::string& action_type,  const std::string& preferred)
{
  if (!ctx.factsheet.has_value())
  {
    return preferred;
  }
  return ctx.factsheet->blocking_type_for(action_type, preferred);
}

bool Vda5050Connector::send_instant_action(const std::string& name, const std::string& action_type,
                                           const std::string& preferred_blocking, const char* label)
{
  std::string topic;
  nlohmann::json msg;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
      return false;
    }
    RobotContext& ctx = *it->second;
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(proto::make_action( action_type, blocking_type_for(ctx, action_type, preferred_blocking)));
    msg = proto::make_instant_actions(ctx.next_instant_actions_header(), ctx.manufacturer, ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, proto::TOPIC_INSTANT_ACTIONS);
  }
  const bool queued = publish_raw(topic, msg.dump());
  if (queued)
  {
    RCLCPP_INFO(_logger, "[VDA5050] %s -> %s", name.c_str(), label);
  }
  else
  {
    RCLCPP_ERROR(_logger, "[VDA5050] %s -> %s NOT published", name.c_str(), label);
  }
  return queued;
}

void Vda5050Connector::set_strict_validation(bool strict)
{
  std::lock_guard<std::mutex> lock(_mutex);
  _strict_validation = strict;
}

bool Vda5050Connector::pause(const std::string& name)
{
  return send_instant_action(name, "startPause", "NONE", "startPause");
}

bool Vda5050Connector::resume(const std::string& name)
{
  return send_instant_action(name, "stopPause", "NONE", "stopPause");
}

std::string Vda5050Connector::init_position(const std::string& name, double x,  double y, double theta,  const std::string& map_id)
{
  std::string topic;
  nlohmann::json msg;
  std::string action_id;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
      RCLCPP_ERROR(_logger, "[VDA5050] init_position: unknown robot '%s'", name.c_str());
      return {};
    }
    RobotContext& ctx = *it->second;

    const auto pose = ctx.transform.to_robot(x, y, theta);
    const auto action = proto::init_position_action( pose[0], pose[1], pose[2], map_id, blocking_type_for(ctx, "initPosition", "NONE"));
    action_id = action.value("actionId", std::string{});
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(action);
    msg = proto::make_instant_actions(ctx.next_instant_actions_header(), ctx.manufacturer,  ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial, proto::TOPIC_INSTANT_ACTIONS);
  }
  if (!publish_raw(topic, msg.dump()))
  {
    RCLCPP_ERROR(_logger, "[VDA5050] %s -> initPosition NOT published", name.c_str());
    return {};
  }
  RCLCPP_INFO(_logger, "[VDA5050] %s -> initPosition (%.2f, %.2f, %.2f) on '%s'", name.c_str(), x, y, theta, map_id.c_str());
  return action_id;
}

bool Vda5050Connector::set_speed_limit(const std::string& name,
                                       std::optional<double> limit)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end())
  {
    return false;
  }
  it->second->speed_limit = limit;
  return true;
}

std::optional<std::pair<std::string, std::string>>
Vda5050Connector::get_action_result(const std::string& name, const std::string& action_id)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end() || !it->second->last_state.has_value())
  {
    return std::nullopt;
  }
  for (const auto& a : it->second->last_state->action_states)
  {
    if (a.value("actionId", std::string{}) == action_id)
    {
      return std::make_pair(a.value("actionStatus", std::string{}), a.value("resultDescription", std::string{}));
    }
  }
  return std::nullopt;
}

std::optional<std::string> Vda5050Connector::get_known_map(const std::string& name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end() || !it->second->last_state.has_value() || it->second->last_state->map_id.empty())
  {
    return std::nullopt;
  }
  return it->second->last_state->map_id;
}

void Vda5050Connector::poll(const std::string& name)
{
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
      return;
    }
    RobotContext& ctx = *it->second;

    if (ctx.factsheet.has_value() || ctx.connected != true ||  ctx.factsheet_requests >= kFactsheetRequestAttempts)
    {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wait = ctx.factsheet_requests == 0 ? kFactsheetFirstWait : kFactsheetRetryWait;
    if (now - ctx.factsheet_wait_since < wait)
    {
      return;
    }
    ctx.factsheet_wait_since = now;
    ++ctx.factsheet_requests;
  }
  send_instant_action(name, "factsheetRequest", "NONE", "factsheetRequest (no factsheet received)");
}

// ─── AGV -> RMF (uplink) ────────────────────────────────────────────────────────

std::optional<RobotData> Vda5050Connector::get_data(const std::string& name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end())
    return std::nullopt;
  const RobotContext& ctx = *it->second;
  if (!ctx.last_state.has_value() || !ctx.last_state->has_position())
    return std::nullopt;

  const auto& s = *ctx.last_state;
  const auto rmf = ctx.transform.to_rmf(*s.x, *s.y, *s.theta);
  RobotData data;
  data.map_name = s.map_id;
  data.position = rmf;
  data.battery_soc = std::clamp(s.battery_soc.value_or(1.0), 0.0, 1.0);
  return data;
}

bool Vda5050Connector::is_command_completed(const std::string& name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end())
  {
    return false;
  }
    
  RobotContext& ctx = *it->second;
  if (!ctx.last_state.has_value() || ctx.current_order_id.empty())
  {
    return false;
  }

  const auto& s = *ctx.last_state;

  if (s.order_id.empty() || s.order_id != ctx.current_order_id)
  {
    return false;
  }
  if (s.driving)
  {
    return false;
  }

  if (s.order_finished(ctx.current_order_id, ctx.target_node_id))
  {
    return true;
  }

  // The order has drained and the robot has stopped, but its lastNodeId differs from the target: report the mismatch once.
  if (s.node_states.empty() && s.edge_states.empty() &&
      !ctx.target_node_id.empty() && s.last_node_id != ctx.target_node_id)
  {
    const std::string key = ctx.current_order_id + "|" + s.last_node_id;
    if (ctx.last_incomplete_key != key)
    {
      ctx.last_incomplete_key = key;
      RCLCPP_WARN(_logger,
                  "[VDA5050] %s drained order '%s' but reports lastNodeId '%s' "
                  "while the order targeted '%s'. Navigation cannot complete "
                  "until the robot echoes the nodeId this adapter sends.",
                  name.c_str(), ctx.current_order_id.c_str(),
                  s.last_node_id.empty() ? "(empty)" : s.last_node_id.c_str(),
                  ctx.target_node_id.c_str());
    }
  }

  return false;
}

std::optional<std::string> Vda5050Connector::get_action_state( const std::string& name, const std::string& action_id)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end() || !it->second->last_state.has_value())
    return std::nullopt;
  for (const auto& a : it->second->last_state->action_states) 
  {
    if (a.value("actionId", std::string{}) == action_id)
      return a.value("actionStatus", std::string{});
  }
  return std::nullopt;
}

bool Vda5050Connector::is_online(const std::string& name, double state_timeout_s)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end())
  {
    return false;
  }
  const RobotContext& ctx = *it->second;
  if (ctx.connected == false)
  {
    return false;
  }
  if (!ctx.last_state.has_value())
  {
    return false;
  }
  const auto age = std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx.last_state_time).count();
  return age <= state_timeout_s;
}

Readiness Vda5050Connector::readiness(const std::string& name, bool tolerate_pause)
{
  bool online = false;
  std::optional<proto::ParsedState> state;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end())
    {
      return {false, "unknown robot"};
    }
    const RobotContext& ctx = *it->second;
    const auto age = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - ctx.last_state_time).count();
    online = ctx.connected != false && ctx.last_state.has_value() && age <= 10.0;
    state = ctx.last_state;
  }
  return evaluate_readiness(online, state, tolerate_pause);
}

std::string Vda5050Connector::current_order_id(const std::string& name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  return it == _robots.end() ? std::string{} : it->second->current_order_id;
}

bool Vda5050Connector::has_active_order_to(const std::string& name,
                                           const std::string& dest_node_id)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end())
  {
    return false;
  }
  const RobotContext& ctx = *it->second;
  if (ctx.current_order_id.empty() || ctx.target_node_id != dest_node_id ||
      !ctx.last_state.has_value() || ctx.last_state->order_id != ctx.current_order_id)
  {
    return false;
  }
  return !ctx.last_state->order_finished(ctx.current_order_id, ctx.target_node_id);
}

}  // namespace vda5050_fleet_adapter
