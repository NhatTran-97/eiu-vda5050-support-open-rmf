#include "vda5050_fleet_adapter/robot_state_machine.hpp"

#include <cmath>
#include <cstdio>
#include <utility>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter {

namespace {
// How long a single navigation may run before we start complaining, and how
// often to repeat the complaint afterwards.
constexpr double kNavWarnAfterSec = 120.0;
constexpr double kNavWarnEverySec = 60.0;
}  // namespace

RobotStateMachine::RobotStateMachine(rclcpp::Logger logger, Vda5050Connector& connector, std::string robot_name)
: _logger(std::move(logger)), _connector(connector), _name(std::move(robot_name))
{
  
}

std::string RobotStateMachine::derive_node_id(const std::string& name,
                                              std::optional<std::size_t> graph_index,
                                              double x, double y)
{
  if (!name.empty())
  {
    return name;

  }
    
  if (graph_index.has_value())
  {
    return "wp_" + std::to_string(*graph_index);
  }

  const auto snap_zero = [](double v) { return std::fabs(v) < 0.005 ? 0.0 : v; };

  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.2f_%.2f", snap_zero(x), snap_zero(y));
  return buf;
}

void RobotStateMachine::on_navigate(
  const EasyFullControl::Destination& destination, EasyFullControl::CommandExecution execution)
{
  const Eigen::Vector3d p = destination.position();
  const std::string node_id = derive_node_id(destination.name(), destination.graph_index(), p.x(), p.y());

  std::lock_guard<std::mutex> lock(_mutex);

  if (_state == State::EXECUTING_ACTION && _action_exec.has_value())
  {

    RCLCPP_ERROR(_logger,
                 "[%s] navigate requested while action '%s' was still "
                 "executing - abandoning that action so RMF is not left "
                 "waiting on it",
                 _name.c_str(), _action_id.c_str());
    _action_exec->error("Superseded by a navigate command before finishing");
    _action_exec.reset();
    _action_id.clear();
  }

  _nav_exec = std::move(execution);
  _state = State::NAVIGATING;
  _nav_started = std::chrono::steady_clock::now();
  _nav_last_warn = _nav_started;

  RCLCPP_INFO(_logger, "[%s] navigate -> (%.2f, %.2f, %.2f) node '%s' map '%s'",
              _name.c_str(), p.x(), p.y(), p.z(), node_id.c_str(),
              destination.map().c_str());

  _connector.navigate(_name, node_id, p.x(), p.y(), p.z(), destination.map(), destination.speed_limit());
}

void RobotStateMachine::on_stop(ConstActivityIdentifierPtr activity)
{
  std::lock_guard<std::mutex> lock(_mutex);

  // A stop arriving mid-action used to be dropped silently, leaving the robot
  // running an action RMF believes it has cancelled.
  if (_state == State::EXECUTING_ACTION && _action_exec.has_value())
  {
    const auto current = _action_exec->identifier();
    if (activity && current && !(*activity == *current))
      return;


    RCLCPP_WARN(_logger,
                "[%s] stop requested during action '%s' - VDA5050 offers no "
                "way to cancel a running instantAction, so the AGV will keep "
                "executing it; RMF is releasing this activity regardless",
                _name.c_str(), _action_id.c_str());
    _action_exec.reset();
    _action_id.clear();
    _state = State::IDLE;
    return;
  }

  if (_state != State::NAVIGATING || !_nav_exec.has_value())
    return;
  const auto current = _nav_exec->identifier();
  if (activity && current && !(*activity == *current))
    return; 

  RCLCPP_INFO(_logger, "[%s] stop", _name.c_str());
  _connector.stop(_name);
  _nav_exec.reset();
  _state = State::IDLE;
}

void RobotStateMachine::on_action(const std::string& category,
                                  const nlohmann::json& description,
                                  RobotUpdateHandle::ActionExecution execution)
{
  std::vector<std::pair<std::string, std::string>> params;
  if (description.is_object()) 
  {
    for (auto it = description.begin(); it != description.end(); ++it)
      params.emplace_back(it.key(), it.value().is_string() ? 
                                                   it.value().get<std::string>()
                                                 : it.value().dump());
  }

  std::lock_guard<std::mutex> lock(_mutex);
  RCLCPP_INFO(_logger, "[%s] action '%s'", _name.c_str(), category.c_str());

  const std::string action_id =
    _connector.execute_instant_action(_name, category, params);
  if (action_id.empty())
  {
    
    RCLCPP_ERROR(_logger,
                 "[%s] action '%s' was not published - robot is not registered "
                 "with the VDA5050 connector; staying IDLE",
                 _name.c_str(), category.c_str());
    return;
  }

  _action_id = action_id;
  _action_exec = std::move(execution);
  _state = State::EXECUTING_ACTION;
}

RobotStateMachine::ConstActivityIdentifierPtr
RobotStateMachine::on_state_update()
{
  std::lock_guard<std::mutex> lock(_mutex);

  if (_state == State::NAVIGATING && _nav_exec.has_value()) 
  {
    if (_connector.is_command_completed(_name)) {
      RCLCPP_INFO(_logger, "[%s] navigation completed", _name.c_str());
      _nav_exec->finished();
      _nav_exec.reset();
      _state = State::IDLE;
      return nullptr;
    }

    const auto now = std::chrono::steady_clock::now();
    const double waiting =
      std::chrono::duration<double>(now - _nav_started).count();
    const double since_warn =
      std::chrono::duration<double>(now - _nav_last_warn).count();
    if (waiting > kNavWarnAfterSec && since_warn > kNavWarnEverySec)
    {
      _nav_last_warn = now;
      RCLCPP_ERROR(_logger,
                   "[%s] navigation still not complete after %.0f s and RMF is "
                   "blocked waiting for it. Check that the robot echoes "
                   "lastNodeId equal to the nodeId this adapter sent, and that "
                   "nodeStates/edgeStates drain.",
                   _name.c_str(), waiting);
    }
    return _nav_exec->identifier();
  }

  if (_state == State::EXECUTING_ACTION && _action_exec.has_value()) 
  {
    const auto status = _connector.get_action_state(_name, _action_id);
    if (status == "FINISHED") 
    {
      RCLCPP_INFO(_logger, "[%s] action %s finished", _name.c_str(),
                  _action_id.c_str());
                  _action_exec->finished();
                  _action_exec.reset();
                  _action_id.clear();
                  _state = State::IDLE;
      return nullptr;
    }
    if (status == "FAILED") 
    {
      RCLCPP_ERROR(_logger, "[%s] action %s FAILED", _name.c_str(), _action_id.c_str());
      _action_exec.reset();  
      _action_id.clear();
      _state = State::IDLE;
      return nullptr;
    }
    return _action_exec->identifier();
  }

  return nullptr;
}

RobotStateMachine::State RobotStateMachine::state()
{
  std::lock_guard<std::mutex> lock(_mutex);
  return _state;
}

}  // namespace vda5050_fleet_adapter
