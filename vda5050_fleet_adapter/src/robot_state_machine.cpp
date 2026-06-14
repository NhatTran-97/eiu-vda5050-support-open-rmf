#include "vda5050_fleet_adapter/robot_state_machine.hpp"

#include <cstdio>
#include <utility>

#include <rclcpp/logging.hpp>

namespace vda5050_fleet_adapter {

RobotStateMachine::RobotStateMachine(rclcpp::Logger logger, Vda5050Connector& connector, std::string robot_name)
: _logger(std::move(logger)), _connector(connector), _name(std::move(robot_name))
{
  
}

std::string RobotStateMachine::derive_node_id(const std::string& name,
                                              std::optional<std::size_t> graph_index,
                                              double x, double y)
{
  if (!name.empty())
    return name;
  if (graph_index.has_value())
    return "wp_" + std::to_string(*graph_index);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.2f_%.2f", x, y);
  return buf;
}

void RobotStateMachine::on_navigate(
  const EasyFullControl::Destination& destination,
  EasyFullControl::CommandExecution execution)
{
  const Eigen::Vector3d p = destination.position();
  const std::string node_id =
    derive_node_id(destination.name(), destination.graph_index(), p.x(), p.y());

  std::lock_guard<std::mutex> lock(_mutex);
  _nav_exec = std::move(execution);
  _state = State::NAVIGATING;

  RCLCPP_INFO(_logger, "[%s] navigate -> (%.2f, %.2f, %.2f) node '%s' map '%s'",
              _name.c_str(), p.x(), p.y(), p.z(), node_id.c_str(),
              destination.map().c_str());

  _connector.navigate(_name, node_id, p.x(), p.y(), p.z(), destination.map(),
                      destination.speed_limit());
}

void RobotStateMachine::on_stop(ConstActivityIdentifierPtr activity)
{
  std::lock_guard<std::mutex> lock(_mutex);
  if (_state != State::NAVIGATING || !_nav_exec.has_value())
    return;
  const auto current = _nav_exec->identifier();
  if (activity && current && !(*activity == *current))
    return;  // stop is for a different (stale) activity

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
  if (description.is_object()) {
    for (auto it = description.begin(); it != description.end(); ++it)
      params.emplace_back(it.key(),
                          it.value().is_string() ? it.value().get<std::string>()
                                                 : it.value().dump());
  }

  std::lock_guard<std::mutex> lock(_mutex);
  RCLCPP_INFO(_logger, "[%s] action '%s'", _name.c_str(), category.c_str());
  _action_id = _connector.execute_instant_action(_name, category, params);
  _action_exec = std::move(execution);
  _state = State::EXECUTING_ACTION;
}

RobotStateMachine::ConstActivityIdentifierPtr
RobotStateMachine::on_state_update()
{
  std::lock_guard<std::mutex> lock(_mutex);

  if (_state == State::NAVIGATING && _nav_exec.has_value()) {
    if (_connector.is_command_completed(_name)) {
      RCLCPP_INFO(_logger, "[%s] navigation completed", _name.c_str());
      _nav_exec->finished();
      _nav_exec.reset();
      _state = State::IDLE;
      return nullptr;
    }
    return _nav_exec->identifier();
  }

  if (_state == State::EXECUTING_ACTION && _action_exec.has_value()) {
    const auto status = _connector.get_action_state(_name, _action_id);
    if (status == "FINISHED") {
      RCLCPP_INFO(_logger, "[%s] action %s finished", _name.c_str(),
                  _action_id.c_str());
      _action_exec->finished();
      _action_exec.reset();
      _action_id.clear();
      _state = State::IDLE;
      return nullptr;
    }
    if (status == "FAILED") {
      RCLCPP_ERROR(_logger, "[%s] action %s FAILED", _name.c_str(),
                   _action_id.c_str());
      _action_exec.reset();  // leave it to RMF to re-issue / recover
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
