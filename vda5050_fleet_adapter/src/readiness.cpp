#include "vda5050_fleet_adapter/readiness.hpp"

namespace vda5050_fleet_adapter {

Readiness evaluate_readiness(bool online,
                             const std::optional<protocol::ParsedState>& state,
                             bool tolerate_pause)
{
  if (!online)
  {
    return {false, "offline"};
  }
  if (!state.has_value())
  {
    return {false, "no state received"};
  }
  if (!state->has_position())
  {
    return {false, "no valid pose"};
  }
  if (!state->operable())
  {
    return {false, "operating mode " + state->operating_mode + " does not accept orders"};
  }
  if (state->safety_state.triggered())
  {
    return {false, "safety stop or field violation active"};
  }
  const std::string fatal = state->first_fatal_error();
  if (!fatal.empty())
  {
    return {false, "FATAL error " + fatal};
  }
  if (state->paused && !tolerate_pause)
  {
    return {false, "AGV reports paused"};
  }
  return {};
}

}  // namespace vda5050_fleet_adapter
