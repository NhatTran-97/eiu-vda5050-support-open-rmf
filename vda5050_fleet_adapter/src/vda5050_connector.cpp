#include "vda5050_fleet_adapter/vda5050_connector.hpp"

#include <unistd.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter {

namespace proto = protocol;

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
       {proto::TOPIC_STATE, proto::TOPIC_CONNECTION, proto::TOPIC_VISUALIZATION})
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
  } else if (ends_with(proto::TOPIC_CONNECTION)) 
  {
    const std::string conn = payload.value("connectionState", std::string{});
    const bool online = (conn == "ONLINE");
    if (ctx->connected != online) 
    {
      if (online)
      {
        RCLCPP_INFO(_logger, "[VDA5050] %s ONLINE", ctx->name.c_str());
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

  {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _robots.find(name);
    if (it == _robots.end()) {
      RCLCPP_ERROR(_logger, "[VDA5050] navigate: unknown robot '%s'",
                   name.c_str());
      return;
    }
    RobotContext& ctx = *it->second;

    dest = ctx.transform.to_robot(x, y, theta);
    base = dest;
    base_id = ctx.last_node_id.empty() ? (ctx.serial + "_start") : ctx.last_node_id;
    if (ctx.last_state.has_value() && ctx.last_state->has_position()) {
      base = {*ctx.last_state->x, *ctx.last_state->y, *ctx.last_state->theta};
    }

    order_id = proto::make_uuid();
    ctx.current_order_id = order_id;
    ctx.target_node_id = dest_node_id;
    header_id = ctx.next_header();
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
                                   base_id, dest_node_id, true, speed_limit));

  const auto order = proto::make_order(header_id, manufacturer, serial, nodes,
                                       edges, order_id, 0);

  // Publish outside any lock: the order was fully built from values copied under
  // the lock above, so no shared state is touched here.
  const std::string order_topic =
    proto::topic(interface_name, manufacturer, serial, proto::TOPIC_ORDER);
  publish_raw(order_topic, order.dump());
  RCLCPP_INFO(_logger, "[VDA5050] %s -> order '%s' to node '%s' (%.2f, %.2f)",
              name.c_str(), order_id.c_str(), dest_node_id.c_str(), dest[0],
              dest[1]);
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
    actions.push_back(proto::cancel_order_action());
    msg = proto::make_instant_actions(ctx.next_header(), ctx.manufacturer,
                                      ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                         proto::TOPIC_INSTANT_ACTIONS);
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

    const auto action = proto::make_action(action_type, "HARD", "", parameters);
    action_id = action.value("actionId", std::string{});
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(action);
    msg = proto::make_instant_actions(ctx.next_header(), ctx.manufacturer,
                                      ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                         proto::TOPIC_INSTANT_ACTIONS);
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
    msg = proto::make_instant_actions(ctx.next_header(), ctx.manufacturer,
                                      ctx.serial, actions);
    topic = proto::topic(ctx.interface_name, ctx.manufacturer, ctx.serial,
                         proto::TOPIC_INSTANT_ACTIONS);
  }
  publish_raw(topic, msg.dump());
}

void Vda5050Connector::publish_raw(const std::string& topic,
                                   const std::string& payload)
{
  if (!_client->is_connected()) {
    RCLCPP_WARN(_logger, "[VDA5050] not connected, dropping publish to %s",
                topic.c_str());
    return;
  }
  auto m = mqtt::make_message(topic, payload);
  m->set_qos(1);
  _client->publish(m);
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
  // ParsedState normalises batteryCharge to a 0.0-1.0 SoC; clamp in case the
  // AGV reports a value outside its nominal range.
  data.battery_soc = std::clamp(s.battery_soc.value_or(1.0), 0.0, 1.0);
  return data;
}

bool Vda5050Connector::is_command_completed(const std::string& name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end())
    return false;
  const RobotContext& ctx = *it->second;
  if (!ctx.last_state.has_value() || ctx.current_order_id.empty())
    return false;

  const auto& s = *ctx.last_state;
  // The AGV must have acknowledged this order (its state orderId matches) before
  // completion can be considered, otherwise a stale state reports a false finish.
  if (s.order_id.empty() || s.order_id != ctx.current_order_id)
    return false;
  if (s.driving)
    return false;
  // Complete when the AGV reaches the target node, or clears all node/edge states
  // for the order (covers the case where it is already at the target).
  if (!ctx.target_node_id.empty() && s.last_node_id == ctx.target_node_id)
    return true;
  return s.order_finished(ctx.current_order_id);
}

std::optional<std::string> Vda5050Connector::get_action_state(
  const std::string& name, const std::string& action_id)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _robots.find(name);
  if (it == _robots.end() || !it->second->last_state.has_value())
    return std::nullopt;
  for (const auto& a : it->second->last_state->action_states) {
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
    return false;
  const RobotContext& ctx = *it->second;
  if (ctx.connected == false)
    return false;
  if (!ctx.last_state.has_value())
    return false;
  const auto age = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - ctx.last_state_time).count();
  return age <= state_timeout_s;
}

}  // namespace vda5050_fleet_adapter
