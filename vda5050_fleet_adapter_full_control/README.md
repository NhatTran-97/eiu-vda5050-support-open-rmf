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

## Reviewer recommendations

| Recommendation | Status |
|---|---|
| Request and consume the factsheet | ✅ Done — `factsheet_handler.cpp` reads capabilities and limits from the AGV's factsheet |
| Drive per-action blocking from the factsheet | ✅ Done — `blocking_type_for()` reads the blocking type from `agvActions`; hardcoded values are only a fallback (`connector.cpp`) |
| Demonstrate multi-robot | ⬜ Not done — needs a second robot identity/config and a demo run |
| Act on connection loss | ✅ Done — `apply_commission()` calls `RobotUpdateHandle::set_commission()`/`decommission()` when VDA5050 state goes stale (`robot_command_handle.cpp`) |
| Pause and resume instead of cancel | ✅ Done — an RMF-initiated stop pauses first; only escalates to `cancelOrder` if no resume arrives before the deadline (`robot_command_handle.cpp`) |
| Add initPosition for re-localization | ✅ Done — `~/<robot>/init_position` service + UI re-localize control (`operator_interface.cpp`) |
| Add validation as the inputs arrive | ✅ Done — startup config validation, plus a runtime check that two consecutive order waypoints have a graph lane between them |
| Model physical actions with mock dispenser and ingestor workcells | ✅ Done — 2 mock workcell scripts + a Delivery task (pickup → wait → dropoff → wait) |
| Try multi-node orders from the /fleet_states path | ✅ Done — `follow_new_path()` sends the whole planned route as one VDA5050 order, not one destination at a time |
| Explore the full_control branch | ✅ Done — built directly on `RobotCommandHandle`/`FleetUpdateHandle` (full control), not `EasyFullControl` |
| Test against a third-party VDA5050 client | ⬜ Not done — only tested against this project's own client (`vda5050_client_adapter`) and mocks |

## Other capabilities

Not from the review above, added along the way:

| Item | Status |
|---|---|
| No-go zone lane closures | ✅ Done — `/lane_closure_requests` → `FleetUpdateHandle::close_lanes()`/`open_lanes()` (`fleet_adapter_full_control.cpp`) |
| mapId mismatch warning | ✅ Done — warns when order `mapId` ≠ AGV's reported `mapId` (`connector.cpp`) |
| Emergency stop (eStop) | ✅ Done — `safetyState.eStop` decommissions the robot (`robot_command_handle.cpp`) |
| Speed limit override | ✅ Done — per-robot `speed_limit.<robot>` ROS parameter, applied live (`operator_interface.cpp`) |
