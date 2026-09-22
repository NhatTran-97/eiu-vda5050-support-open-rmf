#ifndef FLEET_ADAPTER_HPP
#define FLEET_ADAPTER_HPP

namespace vda5050_fleet_adapter_full_control::core {

// Runs the adapter until shutdown; returns non-zero when initialization fails.
int run_fleet_adapter_full_control(int argc, char **argv);

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // FLEET_ADAPTER_HPP
