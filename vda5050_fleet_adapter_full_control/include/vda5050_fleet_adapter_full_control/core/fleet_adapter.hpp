#ifndef FLEET_ADAPTER_HPP
#define FLEET_ADAPTER_HPP

namespace vda5050_fleet_adapter_full_control::core {

// Parses argv, builds the RMF EasyFullControl fleet plus the VDA5050
// Connector, runs the update loop, and blocks until shutdown. Mirrors what
// the old vda5050_fleet_adapter's main() did inline. Returns 1 on any
// startup failure (bad args, unparseable config, RMF/MQTT setup error). On
// a clean shutdown it calls std::_Exit(0) directly (matching the old
// main()) and never actually returns to the caller.
int run_fleet_adapter(int argc, char **argv);

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // FLEET_ADAPTER_HPP
