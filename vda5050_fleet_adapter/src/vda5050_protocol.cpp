#include "vda5050_fleet_adapter/vda5050_protocol.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace vda5050_fleet_adapter::protocol {

namespace {

nlohmann::json header(int header_id, const std::string& manufacturer,
                      const std::string& serial)
{
  return 
  {
    {"headerId", header_id},
    {"timestamp", now_iso()},
    {"version", VERSION},
    {"manufacturer", manufacturer},
    {"serialNumber", serial},
  };
}

}  // namespace

std::string now_iso()
{
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms =
    duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

  std::tm tm_utc{};
  #if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
  #else
    gmtime_r(&t, &tm_utc);
  #endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);

  char out[40];
  std::snprintf(out, sizeof(out), "%s.%03lldZ", buf,
                static_cast<long long>(ms));
  return out;
}

std::string make_uuid()
{
  static thread_local std::mt19937_64 gen{std::random_device{}()};
  std::uniform_int_distribution<uint32_t> d;
  char buf[37];
  const uint32_t a = d(gen), b = d(gen), c = d(gen), e = d(gen);
  std::snprintf(buf, sizeof(buf),"%08x-%04x-4%03x-%04x-%04x%08x",
    a, (b >> 16) & 0xFFFF, b & 0x0FFF,
    ((c >> 16) & 0x3FFF) | 0x8000, c & 0xFFFF, e);
  return buf;
}

std::string topic(const std::string& interface_name,
                  const std::string& manufacturer,
                  const std::string& serial, const std::string& leaf)
{
  return interface_name + "/v2/" + manufacturer + "/" + serial + "/" + leaf;
}

nlohmann::json make_node(const std::string& node_id, int sequence_id,
                         double x, double y, double theta,
                         const std::string& map_id, bool released,
                         double allowed_deviation_xy,
                         double allowed_deviation_theta)
{
  return {
    {"nodeId", node_id},
    {"sequenceId", sequence_id},
    {"released", released},
    {"nodePosition", {
      {"x", x}, {"y", y}, {"theta", theta},
      {"mapId", map_id},
      {"allowedDeviationXY", allowed_deviation_xy},
      {"allowedDeviationTheta", allowed_deviation_theta},
    }},
    {"actions", nlohmann::json::array()},
  };
}

nlohmann::json make_edge(const std::string& edge_id, int sequence_id,
                         const std::string& start_node_id,
                         const std::string& end_node_id, bool released,
                         std::optional<double> max_speed)
{
  nlohmann::json edge = {
    {"edgeId", edge_id},
    {"sequenceId", sequence_id},
    {"released", released},
    {"startNodeId", start_node_id},
    {"endNodeId", end_node_id},
    {"actions", nlohmann::json::array()},
  };
  
  if (max_speed.has_value())
    edge["maxSpeed"] = *max_speed;
  return edge;
}

nlohmann::json make_order(int header_id, const std::string& manufacturer,
                          const std::string& serial, const nlohmann::json& nodes,
                          const nlohmann::json& edges,
                          const std::string& order_id, int order_update_id)
{
  nlohmann::json msg = header(header_id, manufacturer, serial);
  msg["orderId"] = order_id.empty() ? make_uuid() : order_id;
  msg["orderUpdateId"] = order_update_id;
  msg["nodes"] = nodes;
  msg["edges"] = edges;
  return msg;
}

nlohmann::json make_action(
  const std::string& action_type, const std::string& blocking_type,
  const std::string& action_id,
  const std::vector<std::pair<std::string, std::string>>& parameters)
{
  nlohmann::json action = 
  {
    {"actionType", action_type},
    {"actionId", action_id.empty() ? make_uuid() : action_id},
    {"blockingType", blocking_type},
  };

  if (!parameters.empty()) 
  {
    nlohmann::json params = nlohmann::json::array();
    for (const auto& [k, v] : parameters)
    {
      params.push_back({{"key", k}, {"value", v}});
    }
      
    action["actionParameters"] = params;
  }
  return action;
}

nlohmann::json make_instant_actions(int header_id,
                                    const std::string& manufacturer,
                                    const std::string& serial,
                                    const nlohmann::json& actions)
{
  nlohmann::json msg = header(header_id, manufacturer, serial);
  msg["actions"] = actions;
  return msg;
}

nlohmann::json cancel_order_action(const std::string& action_id)
{
  return make_action("cancelOrder", "HARD", action_id);
}

// ─── ParsedState ──────────────────────────────────────────────────────────────

namespace {

template <typename T>
std::optional<T> get_opt(const nlohmann::json& j, const char* key)
{
  if (j.contains(key) && !j.at(key).is_null())
    return j.at(key).get<T>();
  return std::nullopt;
}

std::vector<nlohmann::json> get_array(const nlohmann::json& j, const char* key)
{
  std::vector<nlohmann::json> out;
  if (j.contains(key) && j.at(key).is_array())
    for (const auto& e : j.at(key))
      out.push_back(e);
  return out;
}

}  // namespace

ParsedState::ParsedState(const nlohmann::json& raw)
{
  if (raw.contains("agvPosition") && raw["agvPosition"].is_object()) 
  {
    const auto& pos = raw["agvPosition"];
    x = get_opt<double>(pos, "x");
    y = get_opt<double>(pos, "y");
    theta = get_opt<double>(pos, "theta");
    map_id = pos.value("mapId", std::string{});
    position_initialized = pos.value("positionInitialized", false);
  }

  if (raw.contains("batteryState") && raw["batteryState"].is_object()) {
    if (const auto charge = get_opt<double>(raw["batteryState"], "batteryCharge"))
      battery_soc = *charge / 100.0;  // VDA5050 %: 0–100 -> SoC 0.0–1.0
  }

  order_id = raw.value("orderId", std::string{});
  order_update_id = get_opt<int>(raw, "orderUpdateId");
  last_node_id = raw.value("lastNodeId", std::string{});
  driving = raw.value("driving", false);
  paused = raw.value("paused", false);

  node_states = get_array(raw, "nodeStates");
  edge_states = get_array(raw, "edgeStates");
  action_states = get_array(raw, "actionStates");
  errors = get_array(raw, "errors");
}

bool ParsedState::has_position() const
{
  return x.has_value() && y.has_value() && theta.has_value() &&
         position_initialized;
}

bool ParsedState::order_finished(const std::string& oid) const
{

  if (oid.empty() || order_id.empty() || order_id != oid)
    return false;
  return node_states.empty() && edge_states.empty() && !driving;
}

}  // namespace vda5050_fleet_adapter::protocol
