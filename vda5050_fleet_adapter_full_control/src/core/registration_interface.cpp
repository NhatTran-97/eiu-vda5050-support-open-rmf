#include "vda5050_fleet_adapter_full_control/core/registration_interface.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include <rclcpp/logging.hpp>

#include "vda5050_fleet_adapter_full_control/core/runtime_robots.hpp"

namespace vda5050_fleet_adapter_full_control::core {

namespace {

// How many finished requests are remembered for answering repeats.
constexpr std::size_t kAnsweredKept = 64;

nlohmann::json findings_json(const std::vector<Finding> &findings)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto &f : findings)
    {
        out.push_back({{"code", f.code}, {"message", f.message}});
    }
    return out;
}

nlohmann::json reject(const nlohmann::json &request, const std::string &action, const std::string &code,
                      const std::string &message)
{
    return {{"request_id", request.value("request_id", std::string{})},
            {"fleet", request.value("fleet", std::string{})},
            {"action", action},
            {"name", request.value("name", std::string{})},
            {"ok", false},
            {"needs_confirmation", false},
            {"errors", nlohmann::json::array({{{"code", code}, {"message", message}}})},
            {"warnings", nlohmann::json::array()},
            {"persisted", false}};
}

// Read an optional string field; false when it is present but not a string.
bool read_string(const nlohmann::json &j, const char *key, std::string &out)
{
    if (!j.contains(key) || j.at(key).is_null())
    {
        return true;
    }
    if (!j.at(key).is_string())
    {
        return false;
    }
    out = j.at(key).get<std::string>();
    return true;
}

bool read_number(const nlohmann::json &j, const char *key, double &out)
{
    if (!j.contains(key) || j.at(key).is_null())
    {
        return true;
    }
    if (!j.at(key).is_number())
    {
        return false;
    }
    out = j.at(key).get<double>();
    return true;
}

// Build a RobotSpec from a request; returns an error text, or empty when the fields are well formed.
std::string parse_spec(const nlohmann::json &request, RobotSpec &spec)
{
    if (!read_string(request, "name", spec.name) || !read_string(request, "manufacturer", spec.manufacturer) ||
        !read_string(request, "serial", spec.serial) || !read_string(request, "charger", spec.charger))
    {
        return "name, manufacturer, serial and charger must be strings";
    }
    if (request.contains("responsive_wait") && !request.at("responsive_wait").is_null())
    {
        if (!request.at("responsive_wait").is_boolean())
        {
            return "responsive_wait must be true or false";
        }
        spec.responsive_wait = request.at("responsive_wait").get<bool>();
    }
    if (request.contains("transform") && !request.at("transform").is_null())
    {
        const auto &t = request.at("transform");
        if (!t.is_object() || !read_number(t, "rotation", spec.rotation) || !read_number(t, "scale", spec.scale))
        {
            return "transform must be an object with numeric rotation and scale";
        }
        if (t.contains("translation"))
        {
            const auto &tr = t.at("translation");
            if (!tr.is_array() || tr.size() != 2 || !tr[0].is_number() || !tr[1].is_number())
            {
                return "transform.translation must be two numbers";
            }
            spec.tx = tr[0].get<double>();
            spec.ty = tr[1].get<double>();
        }
    }
    return {};
}

}  // namespace

RegistrationInterface::RegistrationInterface(rclcpp::Node &node, rmf::Connector &connector, RobotManager &manager,
                                             OperatorInterface &operator_interface, Config config)
    : _node(node),
      _connector(connector),
      _manager(manager),
      _operator_interface(operator_interface),
      _config(std::move(config)),
      _started(std::chrono::steady_clock::now())
{
    // Registry and discovery are full snapshots: a late subscriber only needs the last one.
    const auto latched = rclcpp::QoS(1).reliable().transient_local();
    _result_pub = _node.create_publisher<std_msgs::msg::String>(kRegistrationResultTopic, rclcpp::QoS(20).reliable());
    _registry_pub = _node.create_publisher<std_msgs::msg::String>(kRobotRegistryTopic, latched);
    _discovery_pub = _node.create_publisher<std_msgs::msg::String>(kRobotDiscoveryTopic, latched);

    _request_sub = _node.create_subscription<std_msgs::msg::String>(
        kRegistrationRequestTopic, rclcpp::QoS(20).reliable(),
        [this](const std_msgs::msg::String &msg)
        {
            nlohmann::json request = nlohmann::json::parse(msg.data, nullptr, false);
            if (request.is_discarded() || !request.is_object())
            {
                RCLCPP_WARN(_node.get_logger(), "Ignoring a registration request that is not a JSON object");
                return;
            }
            const auto result = handle_request(request);
            if (!result.is_null())
            {
                publish(_result_pub, result);
            }
        });
    _registry_sub = _node.create_subscription<std_msgs::msg::String>(
        kRobotRegistryTopic, latched,
        [this](const std_msgs::msg::String &msg)
        {
            const nlohmann::json registry = nlohmann::json::parse(msg.data, nullptr, false);
            if (!registry.is_discarded())
            {
                on_registry_message(registry);
            }
        });
    _discovery_timer = _node.create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(_config.discovery_period_s)),
        [this]() { poll_discovery(); });

    RCLCPP_INFO(_node.get_logger(),"Robot registration for fleet '%s': requests on %s, results on %s, registry on %s, discovery on %s",
                _config.fleet_name.c_str(), kRegistrationRequestTopic, kRegistrationResultTopic, kRobotRegistryTopic, kRobotDiscoveryTopic);
}

void RegistrationInterface::publish(const rclcpp::Publisher<std_msgs::msg::String>::SharedPtr &pub,const nlohmann::json &message)
{
    std_msgs::msg::String out;
    out.data = message.dump();
    pub->publish(out);
}

std::vector<KnownRobot> RegistrationInterface::own_robots() const
{
    std::vector<KnownRobot> out;
    for (const auto &entry : _manager.snapshot())
    {
        out.push_back({_config.fleet_name, entry->spec.name, entry->spec.manufacturer, entry->spec.serial,entry->spec.charger, entry->retired.load()});
    }
    return out;
}

FleetView RegistrationInterface::fleet_view() const
{
    FleetView view;
    view.limits = _config.limits;
    view.robots = own_robots();
    for (const auto &[fleet, robots] : _other_fleets)
    {
        view.robots.insert(view.robots.end(), robots.begin(), robots.end());
    }
    view.reference_factsheets = _connector.registered_factsheets();
    return view;
}

nlohmann::json RegistrationInterface::registry_json() const
{
    nlohmann::json robots = nlohmann::json::array();
    // Charger name to the robot using it and whether that robot was removed.
    std::map<std::string, std::pair<std::string, bool>> charger_user;
    for (const auto &entry : _manager.snapshot())
    {
        const bool retired = entry->retired.load();
        robots.push_back({{"name", entry->spec.name}, {"manufacturer", entry->spec.manufacturer},
                          {"serial", entry->spec.serial}, {"charger", entry->spec.charger},
                          {"source", entry->spec.from_config ? "config" : "runtime"}, {"retired", retired}});
        if (!entry->spec.charger.empty())
        {
            charger_user[entry->spec.charger] = {entry->spec.name, retired};
        }
    }

    nlohmann::json chargers = nlohmann::json::array();
    if (_config.graph.chargers)
    {
        for (const auto &name : _config.graph.chargers())
        {
            const auto it = charger_user.find(name);
            const bool used = it != charger_user.end();
            chargers.push_back({{"name", name}, {"used_by", used ? nlohmann::json(it->second.first) : nlohmann::json()},
                                {"used_by_removed", used && it->second.second}});
        }
    }

    // The type of this fleet's robots, as one of them declared it.
    nlohmann::json series;
    for (const auto &factsheet : _connector.registered_factsheets())
    {
        if (!factsheet.series_name.empty())
        {
            series = factsheet.series_name;
            break;
        }
    }

    return {{"fleet", _config.fleet_name},
            {"interface", _config.interface_name},
            {"adapter_node", _node.get_name()},
            {"series", series},
            {"limits", {{"footprint_radius", _config.limits.footprint_radius},
                        {"linear_speed", _config.limits.linear_speed},
                        {"linear_acceleration", _config.limits.linear_acceleration},
                        {"tolerance", _config.limits.tolerance}}},
            {"chargers", chargers},
            {"robots", robots}};
}

void RegistrationInterface::publish_registry()
{
    std::lock_guard<std::mutex> lock(_mutex);
    publish_registry_locked();
}

void RegistrationInterface::publish_registry_locked()
{
    const std::string text = registry_json().dump();
    if (_last_registry == text)
    {
        return;
    }
    _last_registry = text;
    std_msgs::msg::String out;
    out.data = text;
    _registry_pub->publish(out);
}

void RegistrationInterface::on_registry_message(const nlohmann::json &registry)
{
    if (!registry.is_object() || !registry.contains("fleet") || !registry["fleet"].is_string() ||
        !registry.contains("robots") || !registry["robots"].is_array())
    {
        return;
    }
    const std::string fleet = registry["fleet"].get<std::string>();
    std::vector<KnownRobot> robots;
    for (const auto &r : registry["robots"])
    {
        if (!r.is_object())
        {
            continue;
        }
        robots.push_back({fleet, r.value("name", std::string{}), r.value("manufacturer", std::string{}),
                          r.value("serial", std::string{}), r.value("charger", std::string{}), r.value("retired", false)});
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (fleet != _config.fleet_name)
    {
        _other_fleets[fleet] = std::move(robots);
    }
}

nlohmann::json RegistrationInterface::handle_request(const nlohmann::json &request)
{
    if (!request.is_object() || request.value("fleet", std::string{}) != _config.fleet_name)
    {
        return nullptr;
    }

    const std::string request_id = request.value("request_id", std::string{});
    std::lock_guard<std::mutex> lock(_mutex);
    if (!request_id.empty())
    {
        const auto earlier = _answered.find(request_id);
        if (earlier != _answered.end())
        {
            return earlier->second;
        }
    }

    const std::string action = request.value("action", std::string{"add"});
    nlohmann::json result;
    if (action == "add")
    {
        result = handle_add(request);
    }
    else if (action == "remove")
    {
        result = handle_remove(request);
    }
    else
    {
        result = reject(request, action, "bad_request", "action must be 'add' or 'remove'");
    }

    if (!request_id.empty())
    {
        _answered[request_id] = result;
        _answered_order.push_back(request_id);
        while (_answered_order.size() > kAnsweredKept)
        {
            _answered.erase(_answered_order.front());
            _answered_order.pop_front();
        }
    }
    return result;
}

bool RegistrationInterface::save_runtime(std::string *error)
{
    std::vector<RobotSpec> specs;
    for (const auto &entry : _manager.snapshot())
    {
        if (!entry->spec.from_config && !entry->retired.load())
        {
            specs.push_back(entry->spec);
        }
    }
    // The first rewrite in a run keeps the file as it was, e.g. entries that were skipped at startup.
    if (!_backed_up)
    {
        if (!backup_runtime_robots(_config.runtime_path, error))
        {
            return false;
        }
        _backed_up = true;
    }
    return save_runtime_robots(_config.runtime_path, specs, error);
}

nlohmann::json RegistrationInterface::handle_add(const nlohmann::json &request)
{
    RobotSpec spec;
    spec.responsive_wait = _config.default_responsive_wait;
    try
    {
        const std::string problem = parse_spec(request, spec);
        if (!problem.empty())
        {
            return reject(request, "add", "bad_request", problem);
        }
    }
    catch (const std::exception &e)
    {
        return reject(request, "add", "bad_request", e.what());
    }
    bool confirm = false;
    if (request.contains("confirm_unverified") && request["confirm_unverified"].is_boolean())
    {
        confirm = request["confirm_unverified"].get<bool>();
    }
    // A dry run reports what the checks say and changes nothing.
    bool dry_run = false;
    if (request.contains("dry_run") && request["dry_run"].is_boolean())
    {
        dry_run = request["dry_run"].get<bool>();
    }

    // The same robot registered again after it was removed comes back as it was.
    const auto removed = _manager.find_removed(spec);

    CandidateFacts candidate;
    if (const auto found = _connector.find_discovered(spec.manufacturer, spec.serial))
    {
        candidate.seen = found->connection_seen;
        candidate.online = found->online;
        candidate.factsheet = found->factsheet;
        candidate.has_state = found->has_state;
        candidate.pose_initialized = found->pose_initialized;
        candidate.x = found->x;
        candidate.y = found->y;
        candidate.theta = found->theta;
        candidate.map_id = found->map_id;
    }

    Verdict verdict;
    if (removed)
    {
        verdict.warnings.push_back({"restored", "'" + spec.name + "' was removed earlier in this session; it will be restored as it was."});
    }
    else
    {
        verdict = validate_new_robot(spec, fleet_view(), candidate, _config.graph, confirm);
    }
    nlohmann::json result = {{"request_id", request.value("request_id", std::string{})},
                             {"fleet", _config.fleet_name},
                             {"action", "add"},
                             {"name", spec.name},
                             {"ok", verdict.ok()},
                             {"needs_confirmation", verdict.needs_confirmation},
                             {"errors", findings_json(verdict.errors)},
                             {"warnings", findings_json(verdict.warnings)},
                             {"persisted", false},
                             {"dry_run", dry_run}};
    if (dry_run)
    {
        return result;
    }
    if (!verdict.ok())
    {
        std::string text;
        for (const auto &e : verdict.errors)
        {
            text += (text.empty() ? "" : " ") + e.message;
        }
        RCLCPP_WARN(_node.get_logger(), "Registration of '%s' (%s/%s) refused: %s", spec.name.c_str(),
                    spec.manufacturer.c_str(), spec.serial.c_str(), text.c_str());
        return result;
    }

    std::shared_ptr<RobotManager::Entry> entry = removed;
    if (removed)
    {
        _manager.reinstate(removed);
    }
    else
    {
        entry = _manager.add(spec);
    }
    _operator_interface.add_robot(
        spec.name, RobotHooks{[command = entry->command]() { return command->pause(); },
                              [command = entry->command]() { return command->resume(); }});

    std::string save_error;
    if (save_runtime(&save_error))
    {
        result["persisted"] = true;
    }
    else
    {
        result["warnings"].push_back(
            {{"code", "not_persisted"},
             {"message", "The robot is added but could not be saved for the next start: " + save_error}});
        RCLCPP_ERROR(_node.get_logger(), "Robot '%s' was not saved: %s", spec.name.c_str(), save_error.c_str());
    }

    publish_registry_locked();
    RCLCPP_INFO(_node.get_logger(), "Robot '%s' (%s/%s) %s fleet '%s' at runtime; charger '%s'%s",
                spec.name.c_str(), spec.manufacturer.c_str(), spec.serial.c_str(), removed ? "restored to" : "added to",
                _config.fleet_name.c_str(), spec.charger.c_str(), result["persisted"].get<bool>() ? "" : " (not saved)");
    return result;
}

nlohmann::json RegistrationInterface::handle_remove(const nlohmann::json &request)
{
    const std::string name = request.value("name", std::string{});
    std::string error;

    if (!_manager.find(name))
    {
        return reject(request, "remove", "unknown_robot", "No robot named '" + name + "' in this fleet.");
    }
    if (!_manager.retire(name, &error))
    {
        return reject(request, "remove", "cannot_remove", error);
    }
    _operator_interface.remove_robot(name);

    nlohmann::json result = {{"request_id", request.value("request_id", std::string{})},
                             {"fleet", _config.fleet_name},
                             {"action", "remove"},
                             {"name", name},
                             {"ok", true},
                             {"needs_confirmation", false},
                             {"errors", nlohmann::json::array()},
                             {"warnings", nlohmann::json::array({{{"code", "rmf_keeps_participant"},
                                                                  {"message", "RMF cannot drop a robot: it stays decommissioned. "
                                                                              "Register it again with the same settings to restore it, or "
                                                                              "restart the adapter to free its name and charger."}}})},
                             {"persisted", false}};
    std::string save_error;
    result["persisted"] = save_runtime(&save_error);
    if (!result["persisted"].get<bool>())
    {
        result["warnings"].push_back(
            {{"code", "not_persisted"}, {"message", "The removal could not be saved: " + save_error}});
    }
    publish_registry_locked();
    return result;
}

void RegistrationInterface::poll_discovery()
{
    _connector.watch_discovered();
    {
        // Keep the registry current, e.g. once a robot's factsheet tells the fleet's type.
        std::lock_guard<std::mutex> lock(_mutex);
        publish_registry_locked();
    }
    // Wait so that the other fleets' registries and this fleet's own robots are seen first.
    if (std::chrono::steady_clock::now() - _started < std::chrono::duration<double>(_config.discovery_grace_s))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    // A removed robot is not registered any more, so it can be offered again.
    std::set<std::string> known;
    for (const auto &r : fleet_view().robots)
    {
        if (!r.retired)
        {
            known.insert(r.manufacturer + "/" + r.serial);
        }
    }

    nlohmann::json robots = nlohmann::json::array();
    std::set<std::string> pending;
    std::string signature;
    for (const auto &d : _connector.discovered())
    {
        const std::string key = d.manufacturer + "/" + d.serial;
        if (!d.online || known.count(key))
        {
            continue;
        }
        pending.insert(key);
        signature += key + (d.factsheet ? ":f" : ":-") + (d.has_state ? "s;" : "-;");
        if (_announced.insert(key).second)
        {
            RCLCPP_INFO(_node.get_logger(), "New robot %s is online on the broker and not registered in any fleet", key.c_str());
        }
        robots.push_back({{"manufacturer", d.manufacturer},
                          {"serial", d.serial},
                          {"series", d.factsheet ? d.factsheet->series_name : std::string{}},
                          {"kinematic", d.factsheet ? d.factsheet->agv_kinematic : std::string{}},
                          {"speed_max", d.factsheet && d.factsheet->speed_max ? nlohmann::json(*d.factsheet->speed_max) : nlohmann::json()},
                          {"pose", d.has_state ? nlohmann::json({{"x", d.x}, {"y", d.y}, {"theta", d.theta}, {"map", d.map_id}, {"initialized", d.pose_initialized}}) : nlohmann::json()}});
    }

    // Removed robots that are online again, with what they were called and where they charge.
    for (const auto &entry : _manager.snapshot())
    {
        if (!entry->retired.load() || !_connector.is_online(entry->spec.name))
        {
            continue;
        }
        const std::string key = entry->spec.manufacturer + "/" + entry->spec.serial;
        pending.insert(key);
        signature += key + ":removed;";
        if (_announced.insert(key).second)
        {
            RCLCPP_INFO(_node.get_logger(), "Removed robot '%s' (%s) is online on the broker; register it again to restore it",
                        entry->spec.name.c_str(), key.c_str());
        }
        const auto data = _connector.get_data(entry->spec.name);
        robots.push_back({{"manufacturer", entry->spec.manufacturer},
                          {"serial", entry->spec.serial},
                          {"series", ""},
                          {"kinematic", ""},
                          {"speed_max", nullptr},
                          {"pose", data ? nlohmann::json({{"x", data->position[0]}, {"y", data->position[1]}, {"theta", data->position[2]}, {"map", data->map_name}, {"initialized", true}}) : nlohmann::json()},
                          {"removed_as", {{"fleet", _config.fleet_name}, {"name", entry->spec.name}, {"charger", entry->spec.charger}}}});
    }

    // Robots that left the list are announced again if they come back.
    for (auto it = _announced.begin(); it != _announced.end();)
    {
        it = pending.count(*it) ? std::next(it) : _announced.erase(it);
    }

    if (_discovery_signature && *_discovery_signature == signature)
    {
        return;
    }
    _discovery_signature = signature;
    publish(_discovery_pub, {{"reporter", _config.fleet_name}, {"interface", _config.interface_name}, {"robots", robots}});
}

}  // namespace vda5050_fleet_adapter_full_control::core
