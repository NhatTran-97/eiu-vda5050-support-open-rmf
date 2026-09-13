# vda5050_fleet_adapter_full_control

Open-RMF fleet adapter for VDA5050 AGVs. Speaks VDA5050 2.1.0 over MQTT to
the robot and RMF's FullControl (`RobotCommandHandle`) interface to RMF, so
a planned multi-waypoint route goes out as one multi-node order instead of
one per waypoint.

See [docs/architecture.md](docs/architecture.md) for features, diagrams,
and the full config reference.

## Prerequisites

- ROS 2 Jazzy + Open-RMF (`ros-jazzy-rmf-fleet-adapter`, `rmf-traffic-ros2`)
- Paho MQTT C++ — not a rosdep key on any target platform, install manually:
  `apt install libpaho-mqttpp-dev libpaho-mqtt-dev`
- A running MQTT broker (Mosquitto) and `mutex_group_supervisor` (stock RMF,
  needed for mutex-protected corridors)

`docker/` has a Jazzy image with all of the above, plus `eiu_fleet_ui`, for
development without touching the host's ROS install.

## Build & run

```bash
colcon build --packages-select vda5050_fleet_adapter_full_control
source install/setup.bash
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py
```

Override the config or nav graph:

```bash
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/abs/config.yaml nav_graph:=/abs/nav_graph.yaml
```

## Configuration

`config/config.yaml` holds both the RMF fleet definition (`rmf_fleet:`) and
the VDA5050/MQTT settings (`vda5050:`) — key reference is in
[docs/architecture.md](docs/architecture.md#configuration-configyaml).
Each robot needs a matching entry under both `rmf_fleet.robots` and
`vda5050.robots`, with the same manufacturer/serial the robot's own
`vda5050_client_adapter` uses — a missing entry fails at startup.

## Testing without hardware

`scripts/mock_mqtt_robot.py` fakes a VDA5050 AGV over MQTT (useful for
multi-robot load testing without extra physical robots).
`scripts/test_dispatch_e2e.py` and `test_pause_resume.py` drive the fleet
adapter end-to-end against it.
