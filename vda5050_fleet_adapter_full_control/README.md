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
| Multi-robot fleet | One process per fleet, one `Connector` state slot + one `RobotCommandHandle` per robot; rejects a duplicate manufacturer/serial pair at startup |
| Runtime robot registration | A robot that shows up on the broker but belongs to no fleet gets reported, and an operator can add it without restarting the adapter. Every request is checked first: type, limits, position on the nav graph, a free charger. Robots added this way are saved and reload at the next start. RMF can't delete a robot, so removing one just decommissions it — register it again unchanged and it comes back, no restart needed. See [Add a robot at runtime](#add-a-robot-at-runtime) |
| Task execution | `follow_new_path` (patrol/delivery/go_to_place), `dock` (parking/charging spots), `PerformAction` (arbitrary instant actions) |
| Task capabilities | Advertised per fleet from config: patrol, delivery, clean, plus any named instant action (e.g. `dock`) |
| Commission tracking | A robot is only offered new tasks while its VDA5050 state is fresh, has a usable pose, is in `AUTOMATIC` or `SEMIAUTOMATIC` mode, reports no eStop, field violation or FATAL error, and is not paused by someone else — any of these failing decommissions it. A pause from the adapter's own traffic hold is tolerated |
| Traffic hold (pause instead of cancel) | An RMF stop sends `startPause` and keeps the order; a new path then updates or replaces it and `stopPause` follows. With no new path within 10 s of the first stop the order is cancelled and the AGV unpaused, unless the operator paused it; this also holds when the AGV has lost its pose, and no order is sent to an AGV without a pose |
| Horizon release | Optional `honor_waypoint_timing`: releases route waypoints to the AGV only as their scheduled time approaches, instead of the whole order at once. Nothing more is released while the AGV is held for a replan |
| Stitching on replan | Optional `stitch_on_replan`: when RMF replans, the new tail is attached to the live order as an order update (same `orderId`) if the new route repeats the part already released; leading points on the AGV's lane, repeated turns and waypoints passed straight through are tolerated. Otherwise the order is replaced. Not attempted while the AGV has no valid pose |
| Operator interface | ROS services/param/topic per robot: pause, resume, speed-limit override, re-localize (`init_position`) |
| Factsheet awareness | Reads the AGV's declared actions, blocking types and limits: each action gets a blocking type from `agvActions`, custom actions it does not declare are rejected (core actions such as `cancelOrder` only warn), and a `factsheetRequest` is sent when none arrived (after 5 s, then every 20 s, up to 3 times) |
| Order validation | Before publishing, hard violations are rejected (non-finite pose, `mapId` not among the maps the AGV reports, more nodes or edges than the factsheet allows) and soft ones only warned (`minOrderInterval`); `strict_validation: false` turns rejection into a warning |
| Stale state filtering | Drops a state message whose `headerId` is not above the last accepted one and whose `timestamp` is not later (a duplicate or a late arrival); a `connection` `ONLINE` message, or `vda5050.stale_state_streak` consecutive drops, starts a new sequence, and `0` turns the filter off |
| Transport security | Optional TLS to the broker (`vda5050.mqtt.tls`): the broker's certificate is checked against a CA file (or the system store) and against its host name, and a client certificate and key can be given for a broker that asks for one; a missing or unreadable file stops the adapter at startup. `username` and `password` may be written as `${VARIABLE}` to read them from the environment; credentials sent without TLS are warned about |
| Input limits | Drops an uplink message larger than `vda5050.mqtt.max_payload_bytes` (default 1 MiB) before its payload is copied or parsed, and logs a problem that repeats with every message (bad JSON, a state missing required fields, an MQTT error) at most once per 30 s per source, with the number of messages held back |
| Cancel confirmation | After `cancelOrder` the adapter watches the state the AGV reports: the cancel's action state (`FINISHED`, `FAILED` or still running) or the order no longer being active answers it; if the AGV neither answers nor drops the order within `vda5050.cancel_confirm_timeout_s` (default 5 s), the cancel is sent again, up to `vda5050.cancel_attempts` times (default 3), then an error is logged. A new order sent by the adapter ends the watch, so a resend never cancels it; a timeout of 0 turns the watch off |
| Fixed-rate update loop | The update loop runs at fixed times (`update_rate_hz`) whatever each pass takes, skips slots it missed and logs a warning when a pass overruns |
| Stuck-order detection | Replans if an AGV never acknowledges a dispatched order's `orderId` within `vda5050.order_stuck_timeout_s` |
| Metrics | Every `vda5050.metrics_period_s` (default 60 s) the adapter logs one `[metrics]` line and publishes the full report as JSON on `~/metrics`: robots online and the oldest state age, messages received and dropped by kind, MQTT reconnects and errors, message handling latency (p50, p99, max), wait for the shared lock, state transit time, and the update loop's pass time and overruns; 0 turns it off (see [docs/architecture.md](docs/architecture.md#metrics)) |
| Tunable thresholds | Offline, stuck-order, traffic-hold, factsheet-request and registration timeouts, and the distances and speeds used to follow a route (`vda5050.state_timeout_s`, `order_stuck_timeout_s`, `waypoint_reached_m`, …) are YAML keys with validated ranges; the defaults are the values the adapter always used |
| Config validation | Fails fast at startup on a numeric setting that is out of range or not a number, bad MQTT settings (including out-of-range connect timeout and reconnect backoff), duplicate identities, out-of-range `vda5050.registration` values, or a nav-graph robot missing from `vda5050.robots` |
| Lane closures (no-go zones) | Subscribes to `/lane_closure_requests`; matching `fleet_name` calls `FleetUpdateHandle::close_lanes()` / `open_lanes()`, so RMF stops routing through those lanes fleet-wide |
| Emergency stop (eStop) | Reads `safetyState.eStop`/`fieldViolation` from the AGV's VDA5050 state; a non-`NONE` value decommissions the robot immediately, same path as commission tracking |
| Operating mode | An `operatingMode` other than `AUTOMATIC` or `SEMIAUTOMATIC` decommissions the robot until it returns |
| Heterogeneous fleets | `EasyFullControl::FleetConfiguration` shares one `profile`/`limits` per fleet, so different robot types (footprint/kinematics) run as separate config files and separate fleet adapter processes, one RMF fleet name each — see [below](#multiple-robot-types-heterogeneous-fleets) |

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
at once. Replace the `TODO` placeholders in `config_amr.yaml` with the real
AMR specs before running against hardware.

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

A robot that is not listed under `vda5050.robots` can join a running fleet.
Nothing is registered automatically: the adapter reports what it sees and a
person decides.

1. **Discovery.** Each adapter watches the `connection` topics of its
   interface. A robot that is online and that no fleet lists is published on
   `/robot_discovery` (after `discovery_grace_s`, so that every fleet has
   announced its own robots first) with the type, speed and pose from its
   factsheet and state. The dashboard shows it under *Needs Attention*.
2. **Request.** The dashboard's *Register* dialog, or
   [`scripts/register_robot.py`](scripts/register_robot.py), sends the fleet,
   a robot name, the broker identity (manufacturer and serial) and a charger.
   `--check` runs every check and adds nothing.
3. **Checks.** The adapter answers with all the errors and warnings at once.
   Any error refuses the request:

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
   | `unverified` | error until confirmed | Something could not be checked: the robot was never seen, is offline, has sent no state or is not localized (`positionInitialized: false`; it joins RMF once it is localized, for instance with *Re-localize* in the dashboard); it has no factsheet (or no size/speed/series in it); or no robot of the fleet has a factsheet to compare types with. Sending `confirm_unverified` adds it anyway and turns each unchecked item into a warning |

4. **Registration.** The robot is added to RMF exactly like a robot from the
   config (`RobotManager`, one `RobotCommandHandle` per robot), gets its own
   operator controls (`pause`, `resume`, `init_position`, `speed_limit.<name>`)
   and is written to the runtime file, so the next start loads it before any
   request arrives. The result says whether it was saved.

The checks compare a robot with *its fleet*: a fleet has one footprint,
speed and kinematics for all its robots (see
[Multiple robot types](#multiple-robot-types-heterogeneous-fleets)), so only a
robot of the same type can join. A robot of another type belongs in another
fleet, which needs its own config file and adapter process. The first robot of
a fleet has nothing to be compared with on type; it needs `confirm_unverified`.

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

`--confirm-unverified` accepts unchecked items; `--rotation`, `--scale` and
`--translation X Y` describe the robot's map frame when it differs from the
fleet's. Exit status: 0 done, 1 refused, 2 no reply.

### Topics

All carry JSON in `std_msgs/String`; a request names its fleet and only that
fleet's adapter answers.

| Topic | Direction | Content |
|:---:|:---:|---|
| `/robot_registration_requests` | client → adapter | `{"request_id", "action": "add"\|"remove", "fleet", "name", "manufacturer", "serial", "charger", "transform": {"rotation", "scale", "translation": [x, y]}, "confirm_unverified", "dry_run"}`; omitted fields take the adapter's defaults. A repeated `request_id` gets the earlier answer, not a second run |
| `/robot_registration_results` | adapter → client | `{"request_id", "fleet", "action", "name", "ok", "needs_confirmation", "errors": [{"code", "message"}], "warnings": [...], "persisted", "dry_run"}` |
| `/robot_registry` | adapter → all (latched) | Per fleet: `fleet`, `interface`, `adapter_node`, `series`, `limits`, `chargers` (`name`, `used_by`, `used_by_removed`) and `robots` (`name`, `manufacturer`, `serial`, `charger`, `source`: `config` or `runtime`, `retired`) |
| `/robot_discovery` | adapter → all (latched) | Per reporting fleet, the robots online on the broker that no fleet lists: `manufacturer`, `serial`, `series`, `kinematic`, `speed_max`, `pose`. A removed robot that is online again is listed too, with `removed_as` (`fleet`, `name`, `charger`) |

### Settings

```yaml
vda5050:
  registration:
    discovery_grace_s: 8.0     # wait after startup before reporting unknown robots
    discovery_period_s: 2.0    # how often the broker is checked
    timeout_s: 30              # time RMF may take to register a robot before a retry
    limit_tolerance: 0.05      # relative margin for speed and acceleration
    # runtime_robots_file: robots.runtime.yaml
```

| Key | Default | Effect |
|:---:|:---:|---|
| `discovery_grace_s` | 8 | Seconds after startup before unknown robots are reported (0–3600) |
| `discovery_period_s` | 2 | Seconds between checks of the broker and of the registry (0.1–3600) |
| `timeout_s` | 30 | Seconds RMF may take to complete a robot's registration before it is tried again (1–600) |
| `limit_tolerance` | 0.05 | Speed and acceleration may differ from the fleet's by this fraction (0–0.5) |
| `runtime_robots_file` | `<config name>.runtime_robots.yaml` next to the config | Where runtime robots are kept; a relative path is taken from the config's directory |

Robots that are not given a `responsive_wait` take the fleet's own
`responsive_wait` default, like the robots in the config.

### The runtime file

`<config name>.runtime_robots.yaml` lists the robots added while the adapter
ran (`manufacturer`, `serial`, `charger`, `responsive_wait`, `transform`). It
is written through a temporary file, so a crash cannot leave it half written,
and the first rewrite of a run keeps the previous file as `*.bak`. At startup
each entry passes the same name, identity, transform and charger checks as a
request; an entry that fails, or a file that is not valid YAML, is reported in
the log with the reason and skipped, and the adapter still starts. Delete an
entry, or the file, to forget a robot. The file is state, not configuration:
keep `*.runtime_robots.yaml*` out of version control.

### Try it without hardware

```bash
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py up      # private broker, RMF core, both fleets
export ROS_DOMAIN_ID=77
ros2 run vda5050_fleet_adapter_full_control register_robot.py discovered    # after about 12 s
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py restart-adapters   # runtime robots come back
ros2 run vda5050_fleet_adapter_full_control registration_sandbox.py down
```

`registration_sandbox.py` starts a broker of its own (a local `mosquitto`, or
`eclipse-mosquitto` through Docker), the RMF schedule and dispatcher, one adapter per
`config/config_*.yaml` and a mock robot for every robot in them. It leaves `tb3_2` out of the
config (`--dynamic` changes that) so it can be added, and starts seven more mock robots
(`S9001`–`S9007` of the first fleet's manufacturer), each built to break one check: a good one on
a free charger, off the nav graph, too slow, too large, without a factsheet, not localized (accepted after confirmation) and of
another type. Poses, speeds and sizes come from the configs and the nav graph. It never touches
the broker or the ROS domain the real system uses (`--port`, `--domain`). Its files (generated
configs, runtime files, logs) go to `--dir`; set `SANDBOX_DIR` once so that every command finds them.

### Removing a robot

`remove` works on robots added at runtime. **RMF has no call that deletes a
robot**, so the adapter can only decommission it (RMF stops giving it tasks)
and stop tracking it: it gets a `cancelOrder` so that it does not finish
an order nobody manages any more, the update loop skips it, and its
pause/resume/`init_position`/`speed_limit` controls answer that it was
removed. RMF keeps the robot's last known position in the traffic
schedule until the adapter restarts, and its name, broker identity and
charger stay reserved until then; take the robot off the floor first. The
runtime file is updated, so the next start does not load it. A robot defined
in the config file has to be removed there, and the adapter restarted.

**Adding a removed robot back.** The robot is still an RMF participant, so
registering it again with the same name, identity, charger, transform and
waiting behaviour restores it as it was, with no restart: the adapter switches
its controls back on, resumes updating it, saves it in the runtime file and
answers with a `restored` warning. While it is online on the broker it is also
offered again (`removed_as` in `/robot_discovery`), and the dashboard fills in
its old name and charger. Any other setting, or another robot on that name or
charger, is refused until the adapter restarts.

## Testing without hardware

`scripts/mock_mqtt_robot.py` fakes a VDA5050 AGV over MQTT (useful for
multi-robot load testing without extra physical robots). Its options set the
identity, start pose and factsheet (`--series`, `--kinematic`, `--agv-class`,
`--speed-max`, `--accel-max`, `--length`, `--width`, `--no-factsheet`,
`--pose-uninitialized`), so each registration check can be provoked on purpose.
`scripts/test_dispatch_e2e.py` and `test_pause_resume.py` drive the fleet
adapter end-to-end against it.

`tools/load_test/` measures the message path under load: `load_probe` (built with the tests) runs the real
`Connector` with N registered robots and a 10 Hz update loop against a broker, `load_gen.py` publishes state and
visualization messages for N simulated AGVs, and `load_run.py` runs both and prints one line of results (CPU,
memory growth, handling latency, lock wait, state age, drops, update-loop overruns). `--timeline` prints each
one-second sample, to watch a broker outage. Point it at a private broker, never at the real one.

## Reviewer recommendations

| Recommendation | Status |
|:---:|---|
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
| Dynamic robot registration (add/remove at runtime) | ✅ Done — [`RegistrationInterface`](src/core/registration_interface.cpp) checks and adds a robot the broker reports, with a matching dashboard flow; a removed robot's name/identity/charger stay reserved and it can be registered again to restore it, no restart either way. See [Add a robot at runtime](#add-a-robot-at-runtime) |
| Test against a third-party VDA5050 client | ⬜ Not done — only tested against this project's own client ([`vda5050_client_adapter`](../vda5050_client_adapter/README.md)) and mocks |

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
