# vda5050_fleet_adapter_full_control

Open-RMF fleet adapter for VDA5050 AGVs. Speaks VDA5050 2.1.0 over MQTT to
the robot and RMF's FullControl (`RobotCommandHandle`) interface to RMF, so
a planned multi-waypoint route goes out as one multi-node order instead of
one per waypoint.

See [docs/architecture.md](docs/architecture.md) for diagrams, sequence
flows, and the full config reference.

## Features

| Area | What it does |
|---|---|
| Multi-robot fleet | One process per fleet, one `Connector` state slot + one `RobotCommandHandle` per robot; rejects a duplicate manufacturer/serial pair at startup |
| Task execution | `follow_new_path` (patrol/delivery/go_to_place), `dock` (parking/charging spots), `PerformAction` (arbitrary instant actions) |
| Task capabilities | Advertised per fleet from config: patrol, delivery, clean, plus any named instant action (e.g. `dock`) |
| Commission tracking | A robot is only offered new tasks while its VDA5050 state is fresh, has a usable pose, is in `AUTOMATIC` or `SEMIAUTOMATIC` mode, reports no eStop, field violation or FATAL error, and is not paused by someone else — any of these failing decommissions it. A pause from the adapter's own traffic hold is tolerated |
| Traffic hold (pause instead of cancel) | An RMF stop sends `startPause` and keeps the order; a new path then updates or replaces it and `stopPause` follows. With no new path within 10 s the order is cancelled and the AGV unpaused, unless the operator paused it |
| Horizon release | Optional `honor_waypoint_timing`: releases route waypoints to the AGV only as their scheduled time approaches, instead of the whole order at once. Nothing more is released while the AGV is held for a replan |
| Stitching on replan | Optional `stitch_on_replan`: when RMF replans, the new tail is attached to the live order as an order update (same `orderId`) if the new route repeats the part already released; leading points on the AGV's lane, repeated turns and waypoints passed straight through are tolerated. Otherwise the order is replaced. Not attempted while the AGV has no valid pose |
| Operator interface | ROS services/param/topic per robot: pause, resume, speed-limit override, re-localize (`init_position`) |
| Factsheet awareness | Reads the AGV's declared actions, blocking types and limits: each action gets a blocking type from `agvActions`, custom actions it does not declare are rejected (core actions such as `cancelOrder` only warn), and a `factsheetRequest` is sent when none arrived (after 5 s, then every 20 s, up to 3 times) |
| Order validation | Before publishing, hard violations are rejected (non-finite pose, `mapId` not among the maps the AGV reports, more nodes or edges than the factsheet allows) and soft ones only warned (`minOrderInterval`); `strict_validation: false` turns rejection into a warning |
| Stuck-order detection | Replans if an AGV never acknowledges a dispatched order's `orderId` within a timeout |
| Config validation | Fails fast at startup on bad MQTT settings, duplicate identities, or a nav-graph robot missing from `vda5050.robots` |
| Lane closures (no-go zones) | Subscribes to `/lane_closure_requests`; matching `fleet_name` calls `FleetUpdateHandle::close_lanes()` / `open_lanes()`, so RMF stops routing through those lanes fleet-wide |
| Emergency stop (eStop) | Reads `safetyState.eStop`/`fieldViolation` from the AGV's VDA5050 state; a non-`NONE` value decommissions the robot immediately, same path as commission tracking |
| Operating mode | An `operatingMode` other than `AUTOMATIC` or `SEMIAUTOMATIC` decommissions the robot until it returns |
| Heterogeneous fleets | `EasyFullControl::FleetConfiguration` shares one `profile`/`limits` per fleet, so different robot types (footprint/kinematics) run as separate config files and separate fleet adapter processes, one RMF fleet name each — see [below](#multiple-robot-types-heterogeneous-fleets) |

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
[docs/architecture.md](docs/architecture.md#configuration-config_tb3yaml--config_amryaml).
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
| Request and consume the factsheet | ✅ Done — [`factsheet_handler.cpp`](src/vda5050/factsheet_handler.cpp) reads capabilities and limits from the AGV's factsheet, and [`poll()`](src/rmf/connector.cpp#L733) sends `factsheetRequest` when none arrived; the checks gate (see validation below). Only exercised against a simulated robot without a retained factsheet |
| Drive per-action blocking from the factsheet | ✅ Done — [`blocking_type_for()`](src/rmf/connector.cpp#L883) reads the blocking type from `agvActions`; hardcoded values are only a fallback |
| Demonstrate multi-robot | ✅ Done — `tb3_fleet` ([`config_tb3.yaml`](config/config_tb3.yaml)) runs two robots (`tb3_1`, `tb3_2`); `amr_fleet` ([`config_amr.yaml`](config/config_amr.yaml)) runs single-robot (`amr_1`) |
| Act on connection loss | ✅ Done — [`apply_commission()`](src/rmf/robot_command_handle.cpp#L866) calls `RobotUpdateHandle::set_commission()`/`decommission()` when VDA5050 state goes stale |
| Pause and resume instead of cancel | ✅ Done — an RMF-initiated stop pauses first ([`stop()`](src/rmf/robot_command_handle.cpp#L347)); only escalates to `cancelOrder` if no new path arrives before the deadline, and [`release_traffic_hold()`](src/rmf/robot_command_handle.cpp#L851) unpauses the AGV afterwards |
| Add initPosition for re-localization | ✅ Done — `~/<robot>/init_position` topic + UI re-localize control ([`on_init_position()`](src/core/operator_interface.cpp#L170)); the AGV's verdict is published on `init_position_result` |
| Add validation as the inputs arrive | ✅ Done — startup config validation ([`config.cpp`](src/core/config.cpp)), a runtime [`has_lane()`](src/rmf/robot_command_handle.cpp#L47) check that two consecutive order waypoints have a graph lane between them, and pre-send order/action validation with a severity split ([`order_validation.cpp`](src/vda5050/order_validation.cpp)): hard violations are rejected, soft ones warned (`strict_validation`) |
| Model physical actions with mock dispenser and ingestor workcells | ✅ Done — [`mock_dispenser.py`](scripts/mock_dispenser.py) + [`mock_ingestor.py`](scripts/mock_ingestor.py) + a Delivery task (pickup → wait → dropoff → wait) |
| Try multi-node orders from the /fleet_states path | ✅ Done — [`follow_new_path()`](src/rmf/robot_command_handle.cpp#L151) sends the whole planned route as one VDA5050 order, not one destination at a time |
| Multi-node orders and order updates (stitching on replan) | ✅ Done — with `stitch_on_replan`, [`replan_route()`](src/rmf/connector.cpp#L292) attaches the replanned tail to the live order ([`route_stitch.cpp`](src/vda5050/route_stitch.cpp)) so the client's stitching runs; verified on a real AMR. A replan that changes the part already released still replaces the order, since VDA5050 cannot withdraw released nodes |
| Explore the full_control branch | ✅ Done — built directly on [`RobotCommandHandle`](include/vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp#L24)/`FleetUpdateHandle` (full control), not `EasyFullControl` |
| Test against a third-party VDA5050 client | ⬜ Not done — only tested against this project's own client ([`vda5050_client_adapter`](../vda5050_client_adapter/README.md)) and mocks |

## Other capabilities

Added along the way; the last four are also listed as done in the M2 follow-up review:

| Item | Status |
|---|---|
| No-go zone lane closures | ✅ Done — `/lane_closure_requests` → [`close_lanes()`/`open_lanes()`](src/core/fleet_adapter_full_control.cpp#L116) |
| mapId mismatch warning | ✅ Done — [`warn_if_map_mismatch()`](src/rmf/connector.cpp#L870) warns when order `mapId` ≠ AGV's reported `mapId` |
| Emergency stop (eStop) | ✅ Done — [`safetyState.eStop`/`field_violation`](src/rmf/robot_command_handle.cpp#L488) decommissions the robot |
| Operating mode (AUTOMATIC / MANUAL) | ✅ Done — a non-automatic `operatingMode` decommissions the robot (`operable()` in [`state_handler.cpp`](src/vda5050/state_handler.cpp)) |
| Speed limit override | ✅ Done — per-robot `speed_limit.<robot>` ROS parameter, applied live ([`speed_limit_parameter()`](src/core/operator_interface.cpp#L32)) |
| Stuck-order replan | ✅ Done — [`is_order_stuck()`](src/rmf/connector.cpp#L1294) asks RMF to replan when an AGV never acknowledges an order |
