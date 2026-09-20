# vda5050_fleet_adapter

Open-RMF **EasyFullControl** fleet adapter that drives VDA5050 AGVs over MQTT. Acts as the VDA5050 master control: receives tasks from Open-RMF, converts each navigation goal into a VDA5050 `order`, and feeds robot `state` back into RMF.

EasyFullControl issues navigation **one destination at a time**, so one VDA5050 order is one destination (base node = current pose, end node = destination) with a fresh `orderId`. This is the simpler, lower-barrier path; multi-node orders, horizon release and stitching live in [`vda5050_fleet_adapter_full_control`](../vda5050_fleet_adapter_full_control/README.md).

See [docs/architecture.md](docs/architecture.md) for diagrams, flows, and the config reference.

## Features

| Area | What it does |
|---|---|
| Navigation | One order per destination; RMF waits for completion before the next. The operator speed cap goes into the edge's `maxSpeed` |
| Actions | `PerformAction` (for example `dock`) is sent as a VDA5050 instant action and tracked to FINISHED or FAILED |
| Factsheet | The retained `factsheet` sets each action's blocking type; custom actions missing from it are rejected (`strict_validation`). A missing factsheet is requested with `factsheetRequest`. An action that conflicts with the AGV's motion or a running HARD-only action is logged as a warning |
| Commissioning | A robot that is offline, has no valid pose, is in a non-automatic operating mode, reports an eStop or field violation, has a FATAL error, or is paused by someone else is decommissioned in RMF and recommissioned when it recovers |
| Pause instead of cancel | When RMF stops a robot the order is paused; a new command reuses or replaces it, and only a hold with no command for 10 s cancels it |
| Operator interface | `<node>/<robot>/init_position` (`PoseWithCovarianceStamped`) with the result on `init_position_result`, services `<node>/<robot>/pause` and `resume`, parameter `speed_limit.<robot>` (m/s, 0 = no cap) |
| Lane closures | `LaneRequest` messages for this fleet close and open lanes in RMF |
| Multiple fleets | One adapter process per fleet, each with its own config file and node name |

## Prerequisites

- ROS 2 Jazzy + Open-RMF (`ros-jazzy-rmf-fleet-adapter`, `rmf-traffic-ros2`)
- Paho MQTT C++: `apt install libpaho-mqttpp-dev libpaho-mqtt-dev`
- A running MQTT broker (Mosquitto)

`docker/` has a Jazzy image with all of the above.

## Build & run

```bash
# 1. Build Docker image and open a shell (first time only)
src/vda5050_fleet_adapter/docker/run.sh

# 2. Inside the container — build
colcon build --packages-select vda5050_fleet_adapter
source install/setup.bash

# 3. Start RMF core
ros2 run rmf_traffic_ros2 rmf_traffic_schedule &
ros2 run rmf_task_ros2 rmf_task_dispatcher &

# 4. Start the fleet adapters (TB3 and AMR)
ros2 launch vda5050_fleet_adapter fleet_adapters.launch.py

# 5. Dispatch a patrol task
python3 src/vda5050_fleet_adapter/scripts/dispatch_patrol.py Patrol_C1

# Inspect MQTT traffic
mosquitto_sub -t 'AMR/v2/#'
```

### Multiple fleets

Each robot type needs its own adapter process, because EasyFullControl reads one fleet per config (footprint and kinematics differ per fleet). `config_tb3.yaml` defines `tb3_fleet` (`tb3_1`, `tb3_2`) and `config_amr.yaml` defines `amr_fleet` (`amr_1`); each adapter needs a unique node name.

```bash
# Both at once
ros2 launch vda5050_fleet_adapter fleet_adapters.launch.py

# Or one per terminal
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py \
    config_file:=config/config_tb3.yaml node_name:=vda5050_fleet_adapter_tb3
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py \
    config_file:=config/config_amr.yaml node_name:=vda5050_fleet_adapter_amr
```

The node name sets the prefix of the operator topics, for example `vda5050_fleet_adapter_tb3/tb3_1/pause`.

## Configuration

Each fleet config file holds the RMF fleet definition (`rmf_fleet:`) and the VDA5050/MQTT settings (`vda5050:`); the key reference is in [docs/architecture.md](docs/architecture.md#configuration-config_tb3yaml--config_amryaml). To add a robot:

1. Add an `is_charger` waypoint for it in `maps/nav_graph.yaml`.
2. Add it under both `rmf_fleet.robots` and `vda5050.robots` in its fleet's config file.

`interface_name`, `manufacturer` and `serial` must match `vda5050_client_adapter` exactly so both ends share the same MQTT topics.

## Testing without hardware

```bash
# Unit tests (no hardware, no MQTT)
colcon test --packages-select vda5050_fleet_adapter

# End-to-end with one simulated robot (no hardware)
python3 src/vda5050_fleet_adapter/scripts/test_dispatch_e2e.py --host localhost \
    --interface AMR --manufacturer ROBOTIS --serial 0001 --target Patrol_C1

# Run simulated robot only (dispatch manually)
python3 src/vda5050_fleet_adapter/scripts/mock_mqtt_robot.py

# Two fleets: mock robots for every robot in both configs, then dispatch one patrol per fleet
python3 src/vda5050_fleet_adapter/scripts/run_mock_fleets.py \
    config/config_amr.yaml config/config_tb3.yaml --host localhost --port 1883
python3 src/vda5050_fleet_adapter/scripts/test_dispatch_e2e.py --no-mock --host localhost \
    --config config/config_amr.yaml --config config/config_tb3.yaml \
    --fleet-target amr_fleet=Patrol_C3 --fleet-target tb3_fleet=Patrol_C1
```

The mock robots reuse the identities of the real robots, so the mock scripts refuse a broker that is not localhost unless `--allow-remote` is given.

## Reviewer recommendations

| Recommendation | Status |
|---|---|
| Port the factsheet and per-action blocking | ✅ Done — [`factsheet.cpp`](src/factsheet.cpp), blocking type chosen in [`vda5050_connector.cpp`](src/vda5050_connector.cpp) |
| Decommission on disconnect | ✅ Done — [`readiness.cpp`](src/readiness.cpp) and [`RobotAdapter::apply_readiness`](src/robot_adapter.cpp) |
| Pause and resume instead of cancel | ✅ Done — `HOLDING` state in [`robot_state_machine.cpp`](src/robot_state_machine.cpp) |
| Port `initPosition` | ✅ Done — [`operator_interface.cpp`](src/operator_interface.cpp) |
| Lane closures, eStop, operating mode, speed override | ✅ Done — lane subscription in [`main.cpp`](src/main.cpp), eStop and mode in the commission decision, speed cap in the connector |
| Multi-node orders, horizon, stitching | ➖ Not ported — EasyFullControl hands over one destination at a time; see `vda5050_fleet_adapter_full_control` |

## Related

- [Root README — system overview](../README.md)
- [VDA5050 Client Adapter](../vda5050_client_adapter/README.md)
- [vda5050_fleet_adapter_full_control](../vda5050_fleet_adapter_full_control/README.md)
