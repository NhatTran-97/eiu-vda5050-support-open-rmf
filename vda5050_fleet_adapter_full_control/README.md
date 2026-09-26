# vda5050_fleet_adapter_full_control

Connects Open-RMF to AGVs that speak VDA5050 2.1 over MQTT.
RMF plans the routes; this adapter turns each route into VDA5050 orders and reports the AGVs' state back to RMF.
It uses RMF's full control interface (`RobotCommandHandle`), so a whole route is sent as one order with many nodes.

Diagrams and message flows: [docs/architecture.md](docs/architecture.md). Config keys: [Configuration](#configuration).

## Key features

| Feature | What it does |
|:---:|---|
| Routes as VDA5050 orders | A route from RMF is sent as one order. With `honor_waypoint_timing`, nodes are released step by step, following RMF's schedule. With `stitch_on_replan`, a new route that still contains the nodes already sent is sent as an update of the same order |
| Tasks | Patrol, delivery, and named instant actions (`PerformAction`). Each fleet enables its tasks in the config |
| Docking and charging | An RMF dock is sent as the instant action set in `dock_actions`. Charging (`charge_at_chargers`) is a demo, tested only with simulated AGVs: `startCharging` at a charger, `stopCharging` before the next order |
| Traffic hold | When RMF asks a robot to stop, the adapter pauses the AGV (`startPause`) and keeps its order. When the new route comes, it updates the order and resumes the AGV (`stopPause`). If no new route comes within `traffic_pause_timeout_s`, the order is cancelled |
| Robot status to RMF | Sends position and battery to RMF. A robot gets new tasks only when it is online, localized, in `AUTOMATIC` or `SEMIAUTOMATIC` mode, has no eStop, field violation or FATAL error, and is not paused by someone else. Each WARNING or FATAL error the AGV reports is raised as an RMF issue (`vda5050_agv_error`) until the AGV clears it |
| Multi-robot fleets | One adapter process per fleet, many robots per fleet. Robots of different sizes or kinematics go in separate fleets — see [Build & run](#build--run) |
| Runtime robot registration | A robot that appears on the broker but is in no fleet is reported. An operator can check it, add it or remove it without restarting — see [Add a robot at runtime](#add-a-robot-at-runtime) |
| Operator controls | Per robot: pause, resume, speed limit, set position (`initPosition`). Per fleet: close and open lanes (`/lane_closure_requests`) |
| VDA5050 2.1 master behavior | Follows the VDA5050 rules for a master controller: resends an order the AGV has not confirmed; waits for a cancel to finish before sending the next order; cancels orders it did not send; replans when the AGV refuses an order (RMF issue `vda5050_order_refused`); checks actions and order size against the AGV's factsheet. Tested with a third-party VDA5050 client ([vda-5050-lib](https://github.com/coatyio/vda-5050-lib.js)) |
| Deployment | Optional MQTT over TLS, passwords from environment variables, and a broker setup with one account per AGV ([fleet_bringup/broker](../fleet_bringup/README.md#secure-broker)). Metrics in the log (`[metrics]`) and as JSON on `~/metrics` |

Current limits: [Limitations](#limitations).

## Package layout

```
config/          fleet configs (config_tb3.yaml, config_amr.yaml, config_tb3_sim.yaml)
maps/            nav graph
launch/          fleet_adapter.launch.py, fleet_adapters.launch.py, mock_workcells.launch.py
src/, include/   C++ adapter, by layer:
  core/            startup, config, update loop, operator controls, runtime registration, metrics report
  rmf/             RobotCommandHandle, Connector (per-robot VDA5050 state), levels, position updates
  vda5050/         order, instantActions, state and factsheet handling, validation, stitching
  mqtt/            Paho MQTT client
  util/            loop pacer, log throttle, metrics counters
scripts/         dispatch and cancel tasks, register_robot.py, mock robot and workcells, sandbox
test/            gtest suites, VDA5050 2.1 schemas, third_party/ virtual AGV (vda-5050-lib)
tools/load_test/ message-path load test
docker/          Jazzy development image
docs/            architecture.md
```

Module diagram: [docs/architecture.md § Component view](docs/architecture.md#component-view).

## Prerequisites

- ROS 2 Jazzy + Open-RMF (`ros-jazzy-rmf-fleet-adapter`, `rmf-traffic-ros2`)
- Paho MQTT C++ (`libpaho-mqtt-dev`, `libpaho-mqttpp-dev`), declared in `package.xml`, so
  `rosdep install --from-paths src --ignore-src` installs it; or `apt install libpaho-mqttpp-dev libpaho-mqtt-dev`
- A running MQTT broker (Mosquitto) and `mutex_group_supervisor` (stock RMF,
  needed for mutex-protected corridors)

`docker/` has a Jazzy image with all of the above, plus `eiu_fleet_ui`, for
development without touching the host's ROS install.

## Build & run

```bash
colcon build --packages-select vda5050_fleet_adapter_full_control
source install/setup.bash
```

Each robot type runs as its own fleet adapter, one terminal each:

```bash
# TB3 fleet
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_tb3.yaml \
    node_name:=vda5050_fleet_adapter_tb3

# AMR fleet
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_amr.yaml \
    node_name:=vda5050_fleet_adapter_amr
```

`fleet_adapters.launch.py` starts the TB3 and AMR fleets in one command.
Without robots: [Demo with virtual AGVs](#demo-with-virtual-agvs).

The adapter's ROS topics and services (operator controls, registration, lane closures) are open to every node in the
same `ROS_DOMAIN_ID`, as is RMF's task API. Run the system on an isolated network, or use SROS2 to restrict access.

## Command line

Every script and launch file in this package, with a link to where it is explained.

| Command | Purpose |
|:---:|---|
| `ros2 launch … fleet_adapter.launch.py` | Run one fleet adapter — [Build & run](#build--run) |
| `ros2 launch … fleet_adapters.launch.py` | Run the TB3 and AMR fleets together |
| `ros2 run … register_robot.py add\|remove\|list\|discovered` | Add/remove/inspect robots at runtime — [Add a robot at runtime](#add-a-robot-at-runtime) |
| `ros2 run … registration_sandbox.py up\|restart-adapters\|status\|down` | Try registration without hardware — [Add a robot at runtime](#add-a-robot-at-runtime) |
| `ros2 run … mock_mqtt_robot.py` | Fake a VDA5050 AGV over MQTT — [Testing without hardware](#testing-without-hardware) |
| `ros2 launch … mock_workcells.launch.py` | Mock dispenser and ingestor for delivery tasks |
| `test/third_party/run_virtual_fleet.sh` | Start third-party virtual AGVs (vda-5050-lib) on the host — [Testing without hardware](#testing-without-hardware) |
| `tools/load_test/load_run.py` | Message-path load test — [Testing without hardware](#testing-without-hardware) |
| `ros2 run … dispatch_patrol.py` / `dispatch_delivery.py` / `cancel_task.py` | Submit or cancel an RMF task from the CLI |
| `scripts/static_analysis.sh` | Run cppcheck, and clang-tidy with `clang_tidy.yaml` when installed, against a `compile_commands.json` |

(`…` = `vda5050_fleet_adapter_full_control`)

## Configuration

Each fleet has one config file (`config/config_*.yaml`) with two sections:
- `rmf_fleet:` — the RMF side: robots, chargers, size and speed limits.
- `vda5050:` — the VDA5050 side: MQTT broker and the manufacturer/serial of each AGV.

List every robot in both sections, with the same name. The manufacturer/serial must match what the AGV sends.
The adapter checks the config at startup. It stops, with the reason in the log, when a value is out of range or of the wrong type, the MQTT settings are incomplete, a robot in `rmf_fleet.robots` has no `vda5050.robots` entry, or two robots share a manufacturer/serial.

The tables list every key the adapter reads, with its default and allowed range. Section headings name the block each key belongs to.

**MQTT connection** (`vda5050.mqtt`)

| Key | Default | Description |
|:---:|:---:|---|
| `host`, `port` | `localhost`, 1883 | Broker address. With TLS the port defaults to 8883 |
| `username`, `password` | — | Broker account. A `${VARIABLE}` is read from the environment |
| `tls.enabled` | false | Connect over TLS (`ssl://`) |
| `tls.ca_file` | system store | PEM file with the CA certificates that sign the broker's certificate |
| `tls.client_cert`, `tls.client_key` | — | PEM files of the client certificate and key, for a broker that asks for one. Set both or neither |
| `tls.verify_hostname` | true | Check that the broker's certificate names the host |
| `qos` | 1 | QoS of `order`, `instantActions` and the subscribed `state`, `visualization` and `factsheet` topics (0–2). `connection` always uses 1. VDA5050 2.1 §6.2 specifies 0 |
| `keep_alive_s` | 60 | Interval of the keep-alive packets that detect a dead connection (1–3600) |
| `connect_timeout_s` | 10 | Time one connect attempt may take (1–120) |
| `reconnect_min_s`, `reconnect_max_s` | 1, 30 | Wait before the first reconnect attempt; it doubles up to the maximum (1–60, 1–600) |
| `max_payload_bytes` | 1048576 | Largest message handled, in bytes; a larger one is dropped unread (1024–268435456) |

**Robots** (`vda5050`)

| Key | Default | Description |
|:---:|:---:|---|
| `interface_name` | `uagv` | First level of every VDA5050 topic. Must not be empty |
| `robots.<name>.manufacturer`, `serial` | — | Identity of the AGV. Required for every robot; must not be empty or contain `/`, `+` or `#` |
| `robots.<name>.transform` | identity | `rotation` (rad), `scale` (not 0) and `translation` `[x, y]` from the RMF frame to the robot's map frame |

**Orders and route following** (`vda5050`)

| Key | Default | Description |
|:---:|:---:|---|
| `update_rate_hz` | 10 | Frequency of the update loop (above 0, up to 100) |
| `honor_waypoint_timing` | false | Release order nodes step by step, following RMF's schedule, instead of all at once |
| `timed_release_max_delay_s` | 0 | Delay RMF tolerates before it interrupts a robot while `honor_waypoint_timing` is on; 0 means no limit (0–3600) |
| `stitch_on_replan` | false | Send a new route as an update of the active order instead of replacing the order |
| `cap_edge_speed_to_fleet` | false | Cap every edge `maxSpeed` at the fleet's nominal speed (`rmf_fleet.limits.linear`). Without the cap, `maxSpeed` is the lane speed limit, lowered by the operator's speed limit |
| `node_deviation_xy_m`, `node_deviation_theta_rad` | 0.5, 3.14 | `allowedDeviationXY` and `allowedDeviationTheta` of every order node (0.01–100, 0.001–3.1416) |
| `waypoint_reached_m` | 0.5 | Distance within which a waypoint counts as reached when the AGV reports no `nodeId` (0.01–10) |
| `same_pose_m`, `same_pose_rad` | 0.05, 0.05 | Poses this close in position and heading count as the same waypoint (0.001–1) |
| `usable_speed_mps` | 0.05 | Measured speeds below this value are replaced by the fleet's nominal speed in arrival estimates (0–1) |
| `early_arrival_warn_s` | 2 | An arrival earlier than RMF's plan by more than this is logged (0–3600) |
| `strict_validation` | true | Reject orders and actions with hard violations. When off, violations are only logged |
| `dock_actions.<dock>` | — | `action` (VDA5050 `actionType`, required) and `parameters` (map of scalars) sent for an RMF dock. A dock that is not listed is sent as an action named after the dock |
| `charge_at_chargers` | false | Send `startCharging` when a path ends at a charger, and `stopCharging` before the next order while the AGV reports charging |

**Order acknowledgement, cancel and traffic hold** (`vda5050`)

| Key | Default | Description |
|:---:|:---:|---|
| `order_ack_timeout_s` | 5 | Time for the AGV state to report a sent order or update before it is sent again unchanged (0.1–3600) |
| `order_resend_attempts` | 2 | Times an unacknowledged order or update is sent again; 0 turns resending off (0–10) |
| `order_stuck_timeout_s` | 15 | Time an order may stay unacknowledged, or a command unsent, before RMF is asked to replan (1–3600) |
| `cancel_confirm_timeout_s` | 5 | Time to wait for the AGV to answer a `cancelOrder`. The replacing order waits for this answer; 0 turns the wait off (0–120) |
| `cancel_attempts` | 3 | Times a `cancelOrder` may be sent for one order, including the first (1–10) |
| `cancel_unknown_orders` | true | Cancel an active order that this adapter did not send before a new order is sent |
| `traffic_pause_timeout_s` | 10 | Time a traffic hold may last before the order is cancelled (0–3600) |

**Robot state** (`vda5050`)

| Key | Default | Description |
|:---:|:---:|---|
| `state_timeout_s` | 10 | Time without a state after which an AGV counts as offline (1–3600) |
| `offline_state_intervals` | 2 | State intervals without a state before an AGV counts as offline, when this is longer than `state_timeout_s` (1–100). The interval is the factsheet's `defaultStateInterval`, or 30 s (VDA5050 2.1 §6.10). A `connection` `OFFLINE` or `CONNECTIONBROKEN` counts as offline at once |
| `stale_state_streak` | 3 | Stale states dropped in a row before the sender counts as restarted; 0 keeps every message (0–100) |
| `factsheet_first_wait_s`, `factsheet_retry_wait_s` | 5, 20 | Wait for the retained factsheet before the first `factsheetRequest`, and between further requests (0–3600, 1–3600) |
| `factsheet_request_attempts` | 3 | `factsheetRequest`s sent to one AGV; 0 turns them off (0–10) |
| `init_position_timeout_s` | 10 | Time to wait for the AGV's answer to an `initPosition` (1–600) |

**Runtime registration** (`vda5050.registration`)

| Key | Default | Description |
|:---:|:---:|---|
| `discovery_grace_s` | 8 | Time after startup before unknown robots are reported (0–3600) |
| `discovery_period_s` | 2 | Time between checks of the broker and of the registry (0.1–3600) |
| `timeout_s` | 30 | Time RMF may take to register a robot before the adapter tries again (1–600) |
| `limit_tolerance` | 0.05 | Allowed relative difference between a new robot's speed and acceleration and the fleet's (0–0.5) |
| `runtime_robots_file` | `<config>.runtime_robots.yaml` | File that keeps robots added at runtime; a relative path is taken from the config's directory |

**Other**

| Key | Default | Description |
|:---:|:---:|---|
| `vda5050.metrics_period_s` | 60 | Time between metrics reports on `~/metrics` and in the log; 0 turns them off (0–3600) |
| `vda5050.ui_websocket_uri` | — | WebSocket address for RMF task events, e.g. for `eiu_fleet_ui` |
| `rmf_fleet.account_for_battery_drain` | false | When off, the battery is reported to RMF as full (SoC 1.0), so RMF never plans a recharge by itself |
| `rmf_fleet.max_merge_waypoint_distance`, `max_merge_lane_distance` | 0.001, 0.3 | How close a robot on a path must be to a waypoint or lane for its position to be reported on it |

## Add a robot at runtime

A new robot can join a running fleet without a restart.
When a robot is online on the broker but is in no fleet, the dashboard shows it under *Needs Attention*.
The operator then adds it from the dashboard or from the command line:

```bash
ros2 run vda5050_fleet_adapter_full_control register_robot.py discovered
ros2 run vda5050_fleet_adapter_full_control register_robot.py add \
    --fleet tb3_fleet --name tb3_3 --manufacturer ROBOTIS --serial 0003 --charger charger_1
ros2 run vda5050_fleet_adapter_full_control register_robot.py remove --fleet tb3_fleet --name tb3_3
```

- The robot must be the same type as the fleet's other robots. The first robot of a fleet has nothing to compare with, so add it with `--confirm-unverified`.
- A removed robot's name, serial and charger stay blocked until the adapter restarts.
- To try it without hardware: `ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py up`
  (starts its own broker, RMF, both fleets and mock robots).

## Testing without hardware

### Demo with virtual AGVs

The virtual AGVs are built on [vda-5050-lib](https://github.com/coatyio/vda-5050-lib.js), a VDA5050 client written by another developer.
They show that the adapter works with a client other than `vda5050_client_adapter`, without hardware.
The broker and the virtual AGVs run on the host (outside Docker). The adapter runs inside the Jazzy container.

```bash
# Host: broker
/usr/sbin/mosquitto -p 18830 -v

# Host: one virtual AGV per charger (Docker, node:20-alpine)
~/ros2_ws/src/vda5050_fleet_adapter_full_control/test/third_party/run_virtual_fleet.sh

# Jazzy container: fleet adapter
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter_full_control/test/third_party/config_virtual_agv.yaml \
    node_name:=vda5050_fleet_adapter_virtual
```

### Automated tests

Unit tests run without a broker or robots:

```bash
colcon test --packages-select vda5050_fleet_adapter_full_control
```

| Test or tool | What it checks |
|:---:|---|
| `test_command_handle_broker` | Command handle and `Connector` against a real MQTT broker, with RMF's `MockAdapter`. Needs `VDA5050_TEST_BROKER`; `MqttReconnect` also needs `VDA5050_TEST_MOSQUITTO`, `MqttTls.*` a TLS broker |
| `test_third_party_agv` | The adapter against a virtual AGV from [vda-5050-lib](https://github.com/coatyio/vda-5050-lib.js) ([test/third_party](test/third_party)): route, horizon updates, pause, cancel and replace, refused order, node actions, charging, `initPosition` |
| `test_schema_samples` + `test_vda5050_schemas` | Every message the adapter sends is checked against the official VDA5050 2.1 JSON schemas ([test/schemas/vda5050_2.1](test/schemas/vda5050_2.1)). Needs `python3-jsonschema` |
| `test_performance` | Time per state message, per update pass, per `follow_new_path` on a large graph. Built, but not run by `ctest` |
| `tools/load_test/` | `load_run.py` runs N simulated AGVs against a real `Connector` and prints CPU, memory, latency and dropped messages |
| `scripts/mock_mqtt_robot.py` | A simple fake AGV for manual tests. `--help` lists the identity, pose and factsheet options; `--strict` follows VDA5050 strictly |

Tests that need a broker are skipped without one. Use a private broker on a free port, not the broker of a running demo.
Run `ctest` from the package's build directory:

```bash
mosquitto -p 18831 &
cd build/vda5050_fleet_adapter_full_control

# Broker tests
VDA5050_TEST_BROKER=tcp://127.0.0.1:18831 VDA5050_TEST_MOSQUITTO=$(which mosquitto) \
    ctest -R test_command_handle_broker

# Third-party AGV test (Node.js 20); serial 0009 to stay clear of the demo AGVs
(cd ../../src/vda5050_fleet_adapter_full_control/test/third_party && npm install && \
    AGV_BROKER=mqtt://127.0.0.1:18831 AGV_SERIAL=0009 node virtual_agv.js &)
VDA5050_TEST_BROKER=tcp://127.0.0.1:18831 VDA5050_THIRD_PARTY_AGV=THIRDPARTY/0009 ctest -R test_third_party_agv
```

TLS test (`MqttTls.*`): checks the secure broker setup of [fleet_bringup/broker](../fleet_bringup/README.md#secure-broker).
The adapter and an AGV connect over TLS, each with its own account; the adapter sends an order, the AGV sends a state,
and the broker blocks the AGV from sending an order.

To run it, create a broker with
`../fleet_bringup/broker/setup_broker.py --out <dir> --agv AMR/ROBOTIS/0001 --host 127.0.0.1 --port <port>` and start it.
Then set `VDA5050_TEST_TLS_BROKER=ssl://127.0.0.1:<port>`, `VDA5050_TEST_TLS_CA=<dir>/certs/ca.crt`,
and the `fleet_master` and `ROBOTIS_0001` passwords from `<dir>/credentials.txt` in
`VDA5050_TEST_TLS_MASTER_PASSWORD` and `VDA5050_TEST_TLS_AGV_PASSWORD`.

## Reviewer recommendations

| Round | Recommendation | Status |
|:---:|:---:|---|
| **Round 1** | Drive per-action blocking from the factsheet | ✅ Done — [`blocking_type_for()`](src/rmf/connector.cpp#L1413) reads the blocking type from `agvActions`; hardcoded values are only a fallback |
| **Round 1** | Demonstrate multi-robot | ✅ Done — `tb3_fleet` ([`config_tb3.yaml`](config/config_tb3.yaml)) runs two robots (`tb3_1`, `tb3_2`); `amr_fleet` ([`config_amr.yaml`](config/config_amr.yaml)) runs one robot (`amr_1`) |
| **Round 1** | Act on connection loss | ✅ Done — [`apply_commission()`](src/rmf/robot_command_handle.cpp#L1292) calls `RobotUpdateHandle::set_commission()`/`decommission()` when VDA5050 state goes stale |
| **Round 1** | Pause and resume instead of cancel | ✅ Done — an RMF-initiated stop pauses first ([`stop()`](src/rmf/robot_command_handle.cpp#L455)); only escalates to `cancelOrder` if no new path arrives before the deadline, and [`release_traffic_hold()`](src/rmf/robot_command_handle.cpp#L1205) unpauses the AGV afterwards |
| **Round 1** | Add initPosition for re-localization | ✅ Done — `~/<robot>/init_position` topic + UI re-localize control ([`on_init_position()`](src/core/operator_interface.cpp#L222)); the AGV's verdict is published on `init_position_result` |
| **Round 1** | Model physical actions with mock dispenser and ingestor workcells | ✅ Done — [`mock_dispenser.py`](scripts/mock_dispenser.py) + [`mock_ingestor.py`](scripts/mock_ingestor.py) + a Delivery task (pickup → wait → dropoff → wait) |
| **Round 1** | Try multi-node orders from the /fleet_states path | ✅ Done — [`follow_new_path()`](src/rmf/robot_command_handle.cpp#L139) sends the whole planned route as one VDA5050 order, not one destination at a time |
| **Round 1** | Explore the full_control branch | ✅ Done — built directly on [`RobotCommandHandle`](include/vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp#L27)/`FleetUpdateHandle` (full control), not `EasyFullControl` |
| **Round 1** | Test against a third-party VDA5050 client | ✅ Done — [`test_third_party_agv`](test/test_third_party_agv.cpp) runs the adapter against the virtual AGV of [vda-5050-lib](https://github.com/coatyio/vda-5050-lib.js) (VDA5050 2.1, validates every inbound message); not yet against a vendor AGV |
| **Round 2** | Request and consume the factsheet | ✅ Done — [`factsheet_handler.cpp`](src/vda5050/factsheet_handler.cpp) reads capabilities and limits from the AGV's factsheet, and [`poll()`](src/rmf/connector.cpp#L1235) sends `factsheetRequest` when none arrived; the validation checks in the next row use it. Only exercised against a simulated robot without a retained factsheet |
| **Round 2** | Add validation as the inputs arrive | ✅ Done — startup config validation ([`config.cpp`](src/core/config.cpp)), a runtime check in [`follow_new_path()`](src/rmf/robot_command_handle.cpp#L260) that warns when two consecutive order waypoints have no graph lane between them, and pre-send order/action validation with a severity split ([`order_validation.cpp`](src/vda5050/order_validation.cpp)): hard violations are rejected, soft ones warned (`strict_validation`) |
| **Round 2** | Multi-node orders and order updates (stitching on replan) | ✅ Done — with `stitch_on_replan`, [`replan_route()`](src/rmf/connector.cpp#L428) attaches the replanned tail to the active order ([`route_stitch.cpp`](src/vda5050/route_stitch.cpp)) so the client's stitching runs; verified on a real AMR. A replan that changes the part already released still replaces the order, since VDA5050 cannot withdraw released nodes |

## Other capabilities

Capabilities beyond the reviewer recommendations. The last four are also marked done in the M2 follow-up review.

| Item | Status |
|:---:|---|
| No-go zone lane closures | ✅ Done — `/lane_closure_requests` → [`close_lanes()`/`open_lanes()`](src/core/fleet_adapter_full_control.cpp#L131) |
| mapId mismatch warning | ✅ Done — [`warn_if_map_mismatch()`](src/rmf/connector.cpp#L1399) warns when order `mapId` ≠ AGV's reported `mapId` |
| Emergency stop (eStop) | ✅ Done — [`safetyState.eStop`/`field_violation`](src/rmf/robot_command_handle.cpp#L695) decommissions the robot |
| Order acknowledgement | ✅ Done — an order the AGV does not confirm in its state is sent again, unchanged ([`resend_unacked_order()`](src/rmf/connector.cpp#L1130), VDA5050 6.6.4.3) |
| Cancel before a new order | ✅ Done — a new order goes out only after the AGV confirms the `cancelOrder` of the old one, or the retries run out ([`cancel_tracker.cpp`](src/vda5050/cancel_tracker.cpp)) |
| Orders from another master | ✅ Done — an order on the AGV that this adapter did not send is cancelled first ([`cancel_unknown_order()`](src/rmf/connector.cpp#L1191)) |
| Secure MQTT | ✅ Done — TLS, one account per AGV and a topic ACL on the broker ([fleet_bringup/broker](../fleet_bringup/README.md#secure-broker)); tested by `MqttTls.*` |
| VDA5050 schema check | ✅ Done — every message the adapter sends is checked against the official VDA5050 2.1 JSON schemas (`test_vda5050_schemas`) |
| Operating mode (AUTOMATIC / MANUAL) | ✅ Done — a non-automatic `operatingMode` decommissions the robot (`operable()` in [`state_handler.cpp`](src/vda5050/state_handler.cpp)) |
| Speed limit override | ✅ Done — per-robot `speed_limit.<robot>` ROS parameter, applied live ([`speed_limit_parameter()`](src/core/operator_interface.cpp#L29)) |
| Stuck-order replan | ✅ Done — [`is_order_stuck()`](src/rmf/connector.cpp#L2058) asks RMF to replan when an AGV never acknowledges an order |
| Dynamic robot registration (add/remove at runtime) | ✅ Done — [`RegistrationInterface`](src/core/registration_interface.cpp) checks and adds a robot that the broker reports, from the dashboard or the CLI. A removed robot can be registered again to restore it. Neither needs a restart. See [Add a robot at runtime](#add-a-robot-at-runtime) |

## Limitations

Cause:
- Hardware: built in software, not yet tested on real robots.
- Site: no multi-floor test site (floors, lift) is available.
- RMF / VDA5050: limits that come from RMF or the VDA5050 standard.
- Optional: a VDA5050 feature the standard leaves optional, not implemented.
- Code: how the code is organised; behaviour is not affected.

| Area | Cause | Status |
|:---:|:---:|---|
| Multiple levels | Site | Orders carry the `mapId` of each RMF level, and levels are mapped to AGV maps ([Levels and maps](docs/architecture.md#levels-and-maps)). Tested on one level only (`tb3_world`). Riding a lift and switching maps between floors are not built yet |
| Charging | Hardware | `startCharging` / `stopCharging` are sent and the AGV's `charging` state is followed. Tested only with simulated AGVs; the real robots have no charging station yet |
| Pick and drop | Hardware | Delivery tasks run with mock dispenser and ingestor workcells. Order nodes can carry pick and drop actions ([Node actions](docs/architecture.md#node-actions-pick-and-drop)), but nothing fills them yet: the robots have no load handling |
| Other vendors' AGVs | Hardware | Tested with a third-party VDA5050 client (vda-5050-lib), not yet with a commercial AGV |
| Removing a robot | RMF | RMF cannot delete a robot, so a removed robot stays in RMF, decommissioned, until the adapter restarts. Its queued tasks are not moved to other robots: cancel them and submit them again. Its name, serial and charger are free again after a restart |
| Lane closed under a robot | RMF | A robot standing on a lane that gets closed stops: RMF can no longer place it on the graph. It continues once the lane is opened again |
| Route changes | VDA5050 | Nodes already released to the AGV cannot be taken back under VDA5050. When a new route from RMF changes those nodes, the adapter cancels the order and sends a new one, and the AGV stops briefly. When the new route changes only the nodes after them, it is sent as an order update and the AGV does not stop |
| Edge trajectory and corridor | Optional | Orders carry nodes and edges only, without a `trajectory` or a `corridor`. The AGV plans its own path between two nodes, so a free-navigating AGV (e.g. TB3 with Nav2) may leave the straight lane that RMF plans traffic on |
| Map distribution | Optional | `downloadMap`, `enableMap` and `deleteMap` are not used. Each AGV's maps must be installed by hand and match the nav graph |
| Large source files | Code | `src/rmf/connector.cpp` (about 2300 lines) and `src/rmf/robot_command_handle.cpp` (about 1400 lines) are long and take time to read. Before larger extensions such as pick and drop or lifts, split them into smaller files, e.g. the per-robot order handling of `Connector` into its own class |
