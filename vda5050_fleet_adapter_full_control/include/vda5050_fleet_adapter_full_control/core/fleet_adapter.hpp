#ifndef FLEET_ADAPTER_HPP
#define FLEET_ADAPTER_HPP

namespace vda5050_fleet_adapter_full_control::core {

// Initializes the RMF fleet and VDA5050 connector, then runs until shutdown.
// Returns a non-zero status when initialization fails.
int run_fleet_adapter_full_control(int argc, char **argv);

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // FLEET_ADAPTER_HPP
