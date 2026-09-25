# vda5050_fleet_adapter_full_control

Open-RMF fleet adapter for VDA5050 AGVs. Speaks VDA5050 2.1.0 over MQTT to
the robot and RMF's FullControl (`RobotCommandHandle`) interface to RMF, so
a planned multi-waypoint route goes out as one multi-node order instead of
one per waypoint.

See [docs/architecture.md](docs/architecture.md) for diagrams, sequence
flows, and the full config reference.

## Features

| Area | What it does |
|:---:|---|
| Multi-robot fleet | One process per fleet; one `Connector` slot and one `RobotCommandHandle` per robot; duplicate manufacturer/serial rejected at startup |
| Runtime robot registration | Robots on the broker that no fleet lists are reported and can be added, checked and saved without a restart; a removed robot is decommissioned and can be restored. See [Add a robot at runtime](#add-a-robot-at-runtime) |
| Task execution | `follow_new_path` (patrol/delivery/go_to_place), `dock` (parking/charging), `PerformAction` (instant actions) |
| Task capabilities | Per fleet from config: patrol, delivery, clean, named instant actions (e.g. `dock`) |
| Commission tracking | New tasks only while the state is fresh, the pose usable, mode `AUTOMATIC`/`SEMIAUTOMATIC`, no eStop, field violation or FATAL error, and not paused by another party (own traffic hold tolerated) |
| Traffic hold | An RMF stop sends `startPause` and keeps the order while it may still run; the new path updates or replaces it, then `stopPause`. No new path within 10 s → order cancelled, AGV unpaused. An operator pause outlasts the hold |
| Horizon release | `honor_waypoint_timing`: waypoints released as their scheduled time approaches; nothing released during a hold |
| Stitching on replan | `stitch_on_replan`: a replanned route that repeats the released part becomes an order update (same `orderId`); otherwise the order is replaced |
| Operator interface | Per robot: pause, resume, speed-limit override, re-localize (`init_position`) |
| Factsheet awareness | Blocking types from `agvActions`; undeclared custom actions rejected (core actions warn); `factsheetRequest` when none arrived (5 s, then every 20 s, 3 times) |
| Order validation | Hard violations rejected (non-finite pose, unknown `mapId`, factsheet node/edge limits), soft ones warned (`minOrderInterval`); `strict_validation: false` warns only |
| Stale state filtering | Drops a state whose `headerId` and `timestamp` are not newer; `connection` `ONLINE` or `stale_state_streak` drops start a new sequence; `0` = off |
| Transport security | Optional TLS (`vda5050.mqtt.tls`): CA, host name, client certificate; `${VARIABLE}` credentials from the environment; warning for credentials without TLS |
| Input limits | Messages above `mqtt.max_payload_bytes` (1 MiB) dropped unparsed; a repeating problem logged at most once per 30 s per source |
| Cancel confirmation | `cancelOrder` resent after `cancel_confirm_timeout_s` (5 s) until the AGV answers or drops the order, up to `cancel_attempts` (3); a new order ends the watch; `0` = off |
| Fixed-rate update loop | Runs at `update_rate_hz`, skips missed slots, warns on overrun |
| Stuck-order detection | Replan when an order is not acknowledged within `order_stuck_timeout_s`, or could not be sent (validation, transport, no pose) |
| Order acknowledgement | Order/update confirmed by `orderId` + `orderUpdateId` in the state; otherwise resent unchanged every `order_ack_timeout_s`, up to `order_resend_attempts` (VDA5050 6.6.4.3); not after a refusal |
| Unknown active orders | An active order this adapter did not send is cancelled (`cancelOrder` with its `orderId`) before any new order (`cancel_unknown_orders`) |
| Replacing an order | A new `orderId` is sent only after `cancelOrder` of the order it replaces, while that order may still run |
| Position reports to RMF | Lane or waypoint within `max_merge_lane_distance` / `max_merge_waypoint_distance` while a path runs; otherwise map + coordinates |
| Input robustness | Wrong JSON types read as absent, non-object array entries dropped, IDs outside 0–4294967295 ignored |
| Metrics | Every `metrics_period_s` (60 s): one `[metrics]` log line and a JSON report on `~/metrics`; `0` = off ([details](docs/architecture.md)) |
| Tunable thresholds | Timeouts, distances and speeds are YAML keys with validated ranges |
| Config validation | Startup fails on out-of-range or non-numeric values, bad MQTT settings, duplicate identities, bad `registration` values, or a nav-graph robot missing from `vda5050.robots` |
| Lane closures | `/lane_closure_requests` → `close_lanes()` / `open_lanes()` for the matching fleet |
| Emergency stop | `safetyState.eStop` ≠ `NONE` or `fieldViolation` decommissions the robot |
| Operating mode | Not `AUTOMATIC`/`SEMIAUTOMATIC` → decommissioned until it returns |
| Heterogeneous fleets | One `profile`/`limits` per fleet, so each robot type runs as its own config and process — see [below](#multiple-robot-types-heterogeneous-fleets) |

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
ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py \
    config_file:=config/config_tb3.yaml node_name:=vda5050_fleet_adapter_tb3 \
    nav_graph:=/abs/nav_graph.yaml   # config_file, node_name, nav_graph all optional
```

Build type defaults to `RelWithDebInfo`; `--cmake-args -DCMAKE_BUILD_TYPE=Debug` to debug.
Every CLI command and script is listed in [Command line](#command-line).

### Multiple robot types (heterogeneous fleets)

`EasyFullControl::FleetConfiguration` applies one `profile`/`limits` to every
robot in a fleet. Different footprints or kinematics need their own config
file and their own adapter process, each with a unique `node_name`.
`fleet_adapters.launch.py` starts both `config_tb3.yaml` and `config_amr.yaml`
at once.

## Command line

Every script and launch file in this package, with a link to where it's explained.

| Command | Purpose |
|:---:|---|
| `ros2 launch … fleet_adapter.launch.py` | Run one fleet adapter — [Build & run](#build--run) |
| `ros2 launch … fleet_adapters.launch.py` | Run the TB3 and AMR fleets together |
| `ros2 run … register_robot.py add\|remove\|list\|discovered` | Add/remove/inspect robots at runtime — [Add a robot at runtime](#add-a-robot-at-runtime) |
| `ros2 run … registration_sandbox.py up\|restart-adapters\|status\|down` | Try registration without hardware — [Try it without hardware](#try-it-without-hardware) |
| `ros2 run … mock_mqtt_robot.py` | Fake a VDA5050 AGV over MQTT — [Testing without hardware](#testing-without-hardware) |
| `ros2 run … test_dispatch_e2e.py` / `test_pause_resume.py` | Drive the adapter end-to-end against a mock robot |
| `ros2 launch … mock_workcells.launch.py` | Mock dispenser + ingestor for delivery tasks |
| `tools/load_test/load_run.py` | Message-path load test — [Testing without hardware](#testing-without-hardware) |
| `scripts/dispatch_patrol.py` / `dispatch_delivery.py` / `cancel_task.py` | Submit or cancel an RMF task from the CLI |
| `scripts/visualize_nav_graph.py` | Publish the nav graph as RViz markers |
| `scripts/static_analysis.sh` | Run cppcheck against a `compile_commands.json` |

(`…` = `vda5050_fleet_adapter_full_control`)

## Configuration

Each fleet config file holds both the RMF fleet definition (`rmf_fleet:`)
and the VDA5050/MQTT settings (`vda5050:`) — key reference is in
[docs/architecture.md](docs/architecture.md#configuration-config_tb3yaml--config_amryaml).
Each robot needs a matching entry under both `rmf_fleet.robots` and
`vda5050.robots`, with the same manufacturer/serial the robot's own
`vda5050_client_adapter` uses — a missing entry fails at startup.

## Add a robot at runtime

A robot not listed under `vda5050.robots` can join a running fleet. Nothing is registered automatically;
an operator decides.

1. **Discovery.** An online robot that no fleet lists is published on `/robot_discovery` (after
   `discovery_grace_s`) with type, speed and pose; the dashboard shows it under *Needs Attention*.
2. **Request.** The dashboard's *Register* dialog or [`scripts/register_robot.py`](scripts/register_robot.py)
   sends fleet, name, manufacturer, serial and charger. `--check` runs the checks only.
3. **Checks.** All errors and warnings are returned at once; any error refuses the request:

   | Code | Severity | Meaning |
   |:---:|:---:|---|
   | `name_invalid`, `name_taken` | error | The name is not 1–64 letters, digits, `_` or `-` starting with a letter or digit, or a robot of any fleet has it |
   | `identity_invalid`, `identity_taken` | error | Manufacturer/serial malformed, or already used by a robot of any fleet (including one removed earlier in this run, unless it is registered again as the same robot) |
   | `transform_invalid` | error | Non-finite value or a scale of 0 |
   | `charger_missing`, `charger_unknown` | error | No charger given, or it is not a charger waypoint of the fleet's nav graph |
   | `charger_taken` | error | Another robot of this fleet uses it, or one removed earlier in this run (RMF keeps that robot's last position until the adapter restarts, so only that robot can be restored on it) |
   | `restored` | warning | The request describes a robot removed earlier in this run; it is brought back as it was instead of being checked as a new one |
   | `charger_shared` | warning | A robot of another fleet uses it |
   | `map_unknown` | error | Its `mapId` is not a map of the fleet's nav graph |
   | `off_graph` | error | Its pose, converted with the transform, does not merge onto a lane or waypoint, so RMF could not register it |
   | `type_mismatch` | error | Series name, kinematics or class differ from the robots already in the fleet |
   | `speed_too_low` | error | Its `speedMax` is below the fleet's planning speed by more than `limit_tolerance`: RMF would expect it to arrive earlier than it can |
   | `speed_higher`, `accel_low` | warning | Faster than, or less agile than, the fleet plans for |
   | `too_large` | error | Half the diagonal of its `length` × `width` exceeds the fleet's footprint radius |
   | `unverified` | error until confirmed | Not checkable: robot unseen, offline, without state, not localized (joins RMF once localized), without factsheet data, or no fleet factsheet to compare. `confirm_unverified` adds it with warnings |

4. **Registration.** Added like a config robot (`RobotManager`, own `RobotCommandHandle` and operator
   controls) and saved to the runtime file for the next start; the result says whether it was saved.

A fleet has one footprint, speed and kinematics ([Multiple robot types](#multiple-robot-types-heterogeneous-fleets)),
so only a robot of the same type can join; another type needs its own fleet config and process. The first robot of a
fleet has no type to compare with and needs `confirm_unverified`.

### Command line

```bash
ros2 run vda5050_fleet_adapter_full_control register_robot.py discovered      # robots waiting for a fleet
ros2 run vda5050_fleet_adapter_full_control register_robot.py list            # robots, chargers, type and limits per fleet
ros2 run vda5050_fleet_adapter_full_control register_robot.py add --check \
    --fleet tb3_fleet --name tb3_3 --manufacturer ROBOTIS --serial 0003 --charger charger_1
ros2 run vda5050_fleet_adapter_full_control register_robot.py add \
    --fleet tb3_fleet --name tb3_3 --manufacturer ROBOTIS --serial 0003 --charger charger_1
ros2 run vda5050_fleet_adapter_full_control register_robot.py remove --fleet tb3_fleet --name tb3_3
```

`--confirm-unverified` accepts unchecked items; `--rotation`, `--scale`, `--translation X Y` set the robot's map
transform. Exit status: 0 done, 1 refused, 2 no reply.

### Topics

JSON in `std_msgs/String`; only the named fleet's adapter answers.

| Topic | Direction | Content |
|:---:|:---:|---|
| `/robot_registration_requests` | client → adapter | `{"request_id", "action": "add"\|"remove", "fleet", "name", "manufacturer", "serial", "charger", "transform": {"rotation", "scale", "translation": [x, y]}, "confirm_unverified", "dry_run"}`; omitted fields take the adapter's defaults. A repeated `request_id` gets the earlier answer, not a second run |
| `/robot_registration_results` | adapter → client | `{"request_id", "fleet", "action", "name", "ok", "needs_confirmation", "errors": [{"code", "message"}], "warnings": [...], "persisted", "dry_run"}` |
| `/robot_registry` | adapter → all (latched) | Per fleet: `fleet`, `interface`, `adapter_node`, `series`, `limits`, `chargers` (`name`, `used_by`, `used_by_removed`) and `robots` (`name`, `manufacturer`, `serial`, `charger`, `source`: `config` or `runtime`, `retired`) |
| `/robot_discovery` | adapter → all (latched) | Per reporting fleet, the robots online on the broker that no fleet lists: `manufacturer`, `serial`, `series`, `kinematic`, `speed_max`, `pose`. A removed robot that is online again is listed too, with `removed_as` (`fleet`, `name`, `charger`) |

### Settings

`vda5050.registration`:

| Key | Default | Effect |
|:---:|:---:|---|
| `discovery_grace_s` | 8 | Seconds after startup before unknown robots are reported (0–3600) |
| `discovery_period_s` | 2 | Seconds between checks of the broker and of the registry (0.1–3600) |
| `timeout_s` | 30 | Seconds RMF may take to complete a robot's registration before it is tried again (1–600) |
| `limit_tolerance` | 0.05 | Speed and acceleration may differ from the fleet's by this fraction (0–0.5) |
| `runtime_robots_file` | `<config name>.runtime_robots.yaml` next to the config | Where runtime robots are kept; a relative path is taken from the config's directory |

A robot without `responsive_wait` takes the fleet default.

### The runtime file

`<config name>.runtime_robots.yaml` lists runtime robots (`manufacturer`, `serial`, `charger`, `responsive_wait`,
`transform`).
- Written through a temporary file; the first rewrite of a run keeps the previous one as `*.bak`.
- At startup each entry gets the request checks; a failing entry or invalid file is logged and skipped.
- Delete an entry or the file to forget a robot. It is state, not configuration: keep `*.runtime_robots.yaml*` out of git.

### Try it without hardware

```bash
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py up      # private broker, RMF core, both fleets
ros2 run vda5050_fleet_adapter_full_control register_robot.py discovered    # after about 12 s
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py restart-adapters   # runtime robots come back
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py down
```

`registration_sandbox.py` starts:
- its own broker (local `mosquitto` or `eclipse-mosquitto` in Docker), RMF schedule and dispatcher;
- one adapter per `config/config_*.yaml` and a mock robot per configured robot, `tb3_2` left out to be added
  (`--dynamic` changes that);
- mock robots `S9001`–`S9007`, one per check: valid, off graph, too slow, too large, no factsheet, not localized,
  other type.

Private `--port` and `--domain`; files go to `--dir` (or `SANDBOX_DIR`).

### Removing a robot

`remove` applies to runtime robots. RMF cannot delete a robot, so the adapter:
- decommissions it (no new tasks) and drops its current path, dock or action;
- sends `cancelOrder` (and `stopPause` if a traffic hold paused it);
- skips it in the update loop; its operator controls answer "removed";
- removes it from the runtime file.

Tasks already queued for it stay queued (cancel them through the task API). RMF keeps its last position, and its
name, identity and charger stay reserved, until the adapter restarts; take the robot off the floor first. A config
robot is removed in the config file plus a restart.

**Adding a removed robot back.** Registering it with the same name, identity, charger, transform and waiting
behaviour restores it without a restart (`restored` warning): controls on, updates resumed, new plan requested,
runtime file saved. While online it is offered again (`removed_as` in `/robot_discovery`). Other settings, or
another robot on that name or charger, are refused until a restart.

## Testing without hardware

| Tool | Purpose |
|:---:|---|
| `scripts/mock_mqtt_robot.py` | Fake VDA5050 AGV over MQTT. Identity, pose and factsheet options (`--series`, `--kinematic`, `--agv-class`, `--speed-max`, `--accel-max`, `--length`, `--width`, `--no-factsheet`, `--pose-uninitialized`); `--strict` follows VDA5050 strictly (new `orderId` refused while active, `orderId` kept after `cancelOrder`) |
| `scripts/test_dispatch_e2e.py`, `test_pause_resume.py` | Drive the adapter end to end against the mock robot |
| `test_command_handle_broker` | `VdaRobotCommandHandle` + `Connector` against a real broker, RMF `MockAdapter`; skipped without a private broker |
| `test_performance` | Cost of a state message, an update pass under load, `follow_new_path` on a large graph, `compute_plan_starts` (built, not run by `ctest`) |
| `tools/load_test/` | `load_run.py` runs `load_probe` (real `Connector`, N robots, 10 Hz loop) against `load_gen.py` (N simulated AGVs) and prints CPU, memory, latency, lock wait, state age, drops, overruns; `--timeline` per second |

Use a private broker only:

```bash
mosquitto -p 18830 &
VDA5050_TEST_BROKER=tcp://127.0.0.1:18830 ctest -R test_command_handle_broker
```

## Reviewer recommendations

| Round | Recommendation | Status |
|:---:|:---:|---|
| **Round 1** | Drive per-action blocking from the factsheet | ✅ Done — [`blocking_type_for()`](src/rmf/connector.cpp#L883) reads the blocking type from `agvActions`; hardcoded values are only a fallback |
| **Round 1** | Demonstrate multi-robot | ✅ Done — `tb3_fleet` ([`config_tb3.yaml`](config/config_tb3.yaml)) runs two robots (`tb3_1`, `tb3_2`); `amr_fleet` ([`config_amr.yaml`](config/config_amr.yaml)) runs single-robot (`amr_1`) |
| **Round 1** | Act on connection loss | ✅ Done — [`apply_commission()`](src/rmf/robot_command_handle.cpp#L866) calls `RobotUpdateHandle::set_commission()`/`decommission()` when VDA5050 state goes stale |
| **Round 1** | Pause and resume instead of cancel | ✅ Done — an RMF-initiated stop pauses first ([`stop()`](src/rmf/robot_command_handle.cpp#L347)); only escalates to `cancelOrder` if no new path arrives before the deadline, and [`release_traffic_hold()`](src/rmf/robot_command_handle.cpp#L851) unpauses the AGV afterwards |
| **Round 1** | Add initPosition for re-localization | ✅ Done — `~/<robot>/init_position` topic + UI re-localize control ([`on_init_position()`](src/core/operator_interface.cpp#L170)); the AGV's verdict is published on `init_position_result` |
| **Round 1** | Model physical actions with mock dispenser and ingestor workcells | ✅ Done — [`mock_dispenser.py`](scripts/mock_dispenser.py) + [`mock_ingestor.py`](scripts/mock_ingestor.py) + a Delivery task (pickup → wait → dropoff → wait) |
| **Round 1** | Try multi-node orders from the /fleet_states path | ✅ Done — [`follow_new_path()`](src/rmf/robot_command_handle.cpp#L151) sends the whole planned route as one VDA5050 order, not one destination at a time |
| **Round 1** | Explore the full_control branch | ✅ Done — built directly on [`RobotCommandHandle`](include/vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp#L24)/`FleetUpdateHandle` (full control), not `EasyFullControl` |
| **Round 1** | Test against a third-party VDA5050 client | ⬜ Not done — only tested against this project's own client ([`vda5050_client_adapter`](../vda5050_client_adapter/README.md)) and mocks |
| **Round 2** | Request and consume the factsheet | ✅ Done — [`factsheet_handler.cpp`](src/vda5050/factsheet_handler.cpp) reads capabilities and limits from the AGV's factsheet, and [`poll()`](src/rmf/connector.cpp#L733) sends `factsheetRequest` when none arrived; the checks gate (see validation below). Only exercised against a simulated robot without a retained factsheet |
| **Round 2** | Add validation as the inputs arrive | ✅ Done — startup config validation ([`config.cpp`](src/core/config.cpp)), a runtime [`has_lane()`](src/rmf/robot_command_handle.cpp#L47) check that two consecutive order waypoints have a graph lane between them, and pre-send order/action validation with a severity split ([`order_validation.cpp`](src/vda5050/order_validation.cpp)): hard violations are rejected, soft ones warned (`strict_validation`) |
| **Round 2** | Multi-node orders and order updates (stitching on replan) | ✅ Done — with `stitch_on_replan`, [`replan_route()`](src/rmf/connector.cpp#L292) attaches the replanned tail to the live order ([`route_stitch.cpp`](src/vda5050/route_stitch.cpp)) so the client's stitching runs; verified on a real AMR. A replan that changes the part already released still replaces the order, since VDA5050 cannot withdraw released nodes |

## Other capabilities

Added along the way; the last four are also listed as done in the M2 follow-up review:

| Item | Status |
|:---:|---|
| No-go zone lane closures | ✅ Done — `/lane_closure_requests` → [`close_lanes()`/`open_lanes()`](src/core/fleet_adapter_full_control.cpp#L116) |
| mapId mismatch warning | ✅ Done — [`warn_if_map_mismatch()`](src/rmf/connector.cpp#L870) warns when order `mapId` ≠ AGV's reported `mapId` |
| Emergency stop (eStop) | ✅ Done — [`safetyState.eStop`/`field_violation`](src/rmf/robot_command_handle.cpp#L488) decommissions the robot |
| Operating mode (AUTOMATIC / MANUAL) | ✅ Done — a non-automatic `operatingMode` decommissions the robot (`operable()` in [`state_handler.cpp`](src/vda5050/state_handler.cpp)) |
| Speed limit override | ✅ Done — per-robot `speed_limit.<robot>` ROS parameter, applied live ([`speed_limit_parameter()`](src/core/operator_interface.cpp#L32)) |
| Stuck-order replan | ✅ Done — [`is_order_stuck()`](src/rmf/connector.cpp#L1294) asks RMF to replan when an AGV never acknowledges an order |
| Dynamic robot registration (add/remove at runtime) | ✅ Done — [`RegistrationInterface`](src/core/registration_interface.cpp) checks and adds a robot the broker reports, with a matching dashboard flow; a removed robot's name/identity/charger stay reserved and it can be registered again to restore it, no restart either way. See [Add a robot at runtime](#add-a-robot-at-runtime) |
