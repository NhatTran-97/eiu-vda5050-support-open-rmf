#pragma once

#include <optional>
#include <string>

#include "vda5050_fleet_adapter/vda5050_protocol.hpp"

namespace vda5050_fleet_adapter {

// Whether an AGV can take new tasks from RMF, and the reason when it cannot.
struct Readiness
{
  bool ready = true;
  std::string reason;
};

// Decide readiness from connectivity and the latest VDA5050 state.
// tolerate_pause ignores a pause the adapter itself requested.
Readiness evaluate_readiness(bool online,
                             const std::optional<protocol::ParsedState>& state,
                             bool tolerate_pause = false);

}  // namespace vda5050_fleet_adapter
