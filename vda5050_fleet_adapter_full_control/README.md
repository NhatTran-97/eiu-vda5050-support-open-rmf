# vda5050_fleet_adapter_full_control

Open-RMF fleet adapter for VDA5050 AGVs. Speaks VDA5050 2.1.0 over MQTT to
the robot and RMF's FullControl (`RobotCommandHandle`) interface to RMF, so
a planned multi-waypoint route goes out as one multi-node order instead of
one per waypoint.

See [docs/architecture.md](docs/architecture.md) for diagrams, sequence
flows, and the full config reference.

## Features

- Multi-node orders — a planned route goes out as one VDA5050 order, not one destination per waypoint.
- Task execution: patrol/delivery/go-to-place (`follow_new_path`), `dock`, and arbitrary `PerformAction` instant actions.
- Commission tracking — a robot is only offered new tasks while its VDA5050 state is fresh *and* has a usable pose.
- Factsheet awareness — reads the AGV's declared speed/array-length/order-interval limits and per-action blocking type; warns before exceeding them.
- Operator interface — per-robot pause, resume, speed-limit override, and re-localize (`init_position`), exposed as ROS services/param/topic.
- No-go zone lane closures — `/lane_closure_requests` closes/opens lanes fleet-wide via `FleetUpdateHandle`.
- Emergency stop — `safetyState.eStop`/`fieldViolation` decommissions the robot immediately.
- Stuck-order detection and replan if an AGV never acknowledges a dispatched order.
- Multiple heterogeneous fleets (different robot types/footprints) via separate config files and processes — see below.
- Config validation at startup: bad MQTT settings, duplicate robot identities, or a nav-graph robot missing from `vda5050.robots` all fail fast instead of at runtime.

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
    config_file:=/abs/config_tb3.yaml nav_graph:=/abs/nav_graph.yaml
```

### Multiple robot types (heterogeneous fleets)

`EasyFullControl::FleetConfiguration` applies a single shared `profile`
(footprint/vicinity) and `limits` to every robot in a fleet, so robots with
different footprints/kinematics must NOT share one `rmf_fleet:` block. Give
each robot type its own config file and run one fleet adapter process per
type, each with a unique ROS 2 node name:

```bash
# Terminal 1 — TB3 fleet
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=config/config_tb3.yaml node_name:=vda5050_fleet_adapter_tb3

# Terminal 2 — AMR fleet
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=config/config_amr.yaml node_name:=vda5050_fleet_adapter_amr
```

Or launch both at once:

```bash
ros2 launch vda5050_fleet_adapter_full_control fleet_adapters.launch.py
```

`config/config_tb3.yaml` is the TB3 fleet (`tb3_fleet`); `config/config_amr.yaml`
is the AMR fleet (`amr_fleet`) — replace the `TODO` placeholder
profile/limits/mechanical_system values in the latter with the real AMR
specs before running against hardware.

## Configuration

Each fleet config file holds both the RMF fleet definition (`rmf_fleet:`)
and the VDA5050/MQTT settings (`vda5050:`) — key reference is in
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
| Demonstrate multi-robot | ✅ Done — `tb3_fleet` (`config_tb3.yaml`) runs two robots (`tb3_1`, `tb3_2`); `amr_fleet` (`config_amr.yaml`) runs single-robot (`amr_1`) |
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
