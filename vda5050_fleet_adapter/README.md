# vda5050_fleet_adapter

A C++ Open-RMF **EasyFullControl** fleet adapter that drives **VDA5050** AGVs over
MQTT. The adapter is the VDA5050 **master**: it receives tasks from Open-RMF,
turns each navigation goal into a VDA5050 `order`, and feeds robot `state` back
into RMF. RMF Core is unchanged, and the robot side talks pure VDA5050 over MQTT,
so this adapter can run on a different ROS distribution than the robots.

## Why EasyFullControl

EasyFullControl issues navigation **one destination at a time**: RMF hands the
adapter a single `Destination` and waits for `execution.finished()` before the
next. Therefore **one VDA5050 order = one destination** (base node = current
pose, end node = the destination), with a fresh `orderId` each time and no
overlapping orders. This removes the multi-node order / stitch / order-update
machinery entirely. EasyFullControl also provides battery, traffic and task
planning out of the box via `from_config_files`.

## System context

```
                       ┌─────────────────┐
                       │   Open-RMF core │   (schedule, dispatcher, task API)
                       └───────┬─────────┘
              BidRequest /     │   ▲  BidResponse /
              DispatchRequest  ▼   │  FleetState
                   ┌───────────────────────────┐
                   │   vda5050_fleet_adapter    │   THIS package (Jazzy, C++)
                   └───────────┬───────────────┘
                       VDA5050 │ JSON over MQTT
                               ▼
                    ┌──────────────────────┐
                    │   MQTT broker         │   (Mosquitto)
                    └──────────┬───────────┘
                               │ VDA5050 JSON
                   ┌───────────▼─────────────┐
                   │ vda5050_client_adapter  │   robot side (separate packages)
                   │ tb3_vda5050_bridge      │
                   │ Nav2                    │
                   └─────────────────────────┘
```

## Internal architecture

```
            ┌──────────────────────── main.cpp ───────────────────────┐
            │ Adapter::make + add_easy_fleet(from_config_files)         │
            │ parse `vda5050:` block; build Vda5050Connector + robots   │
            │ update loop (N Hz): VDA5050 state -> RMF (add/update)     │
            └───────┬───────────────────────────────────┬─────────────┘
                    │ RobotCallbacks (navigate/stop/act) │ EasyRobotUpdateHandle::update
                    ▼                                     ▲
              RobotAdapter ──────── owns ──────── RobotStateMachine
                    │                              (IDLE / NAVIGATING / EXECUTING_ACTION)
                    └──────────────── Vda5050Connector (1 MQTT conn, N robots)
                                      ├─ vda5050_protocol  (build/parse JSON, pure)
                                      └─ transform         (RMF<->robot frame, pure)
```

## Files

### Source — core library (`vda5050_fleet_adapter_core`)

| File | Responsibility |
|---|---|
| `include/.../transform.hpp` | 2D affine transform between the RMF nav-graph frame and a robot map frame (`to_robot` / `to_rmf`). Header-only, pure. Configured per robot in `config.yaml`. |
| `include/.../vda5050_protocol.hpp`<br>`src/vda5050_protocol.cpp` | Transport-agnostic VDA5050 message layer. Builds `order` / `instantActions` (`make_node`, `make_edge`, `make_order`, `make_action`, `make_instant_actions`, `cancel_order_action`), generates UUIDs and ISO-8601 timestamps, composes topic strings, and parses incoming `state` into the read-only `ParsedState` view (position, battery SoC, orderId, lastNodeId, driving, node/edge/action states, errors). No MQTT, no RMF — unit-testable in isolation. |
| `include/.../vda5050_connector.hpp`<br>`src/vda5050_connector.cpp` | Owns one MQTT connection (paho) and a per-robot `RobotContext`. Downlink (RMF → AGV): `navigate` (publish a single-destination order), `stop` (publish cancelOrder), `execute_instant_action`, `request_state`. Uplink (AGV → RMF): caches each robot’s latest `state`/`connection`, exposes `get_data` (position + battery in RMF frame), `is_command_completed`, `get_action_state`, `is_online`. Implements the `mqtt::callback` interface (connect / connection-lost / message-arrived) and re-subscribes on reconnect. Thread-safe via an internal mutex. |
| `include/.../robot_state_machine.hpp`<br>`src/robot_state_machine.cpp` | Explicit per-robot command lifecycle: `IDLE → NAVIGATING → IDLE` and `IDLE → EXECUTING_ACTION → IDLE`. Translates RMF callbacks into connector calls, holds the active `CommandExecution`, and fires `finished()` once the cached state shows the order/action complete. `derive_node_id` maps an RMF destination to a VDA5050 nodeId. |
| `include/.../robot_adapter.hpp`<br>`src/robot_adapter.cpp` | Bridges ONE EasyFullControl robot to the connector through its state machine. Builds the `RobotCallbacks` (navigate / stop / action) handed to RMF, holds the `EasyRobotUpdateHandle`, and pushes each state tick into RMF via `update`. `added()` reports whether the robot has joined the fleet. |

### Source — node executable (`fleet_adapter`)

| File | Responsibility |
|---|---|
| `src/main.cpp` | Entry point and wiring. Parses `-c <config> -n <nav_graph>`, creates the RMF `Adapter`, builds the EasyFullControl fleet from `from_config_files`, reads the `vda5050:` config block (interface, update rate, MQTT, per-robot identity + transform), constructs the `Vda5050Connector` and one `RobotAdapter` per robot, and runs the update loop that adds each robot to the fleet once a valid state arrives and thereafter pushes state into RMF. |

### Configuration & assets

| File | Responsibility |
|---|---|
| `config/config.yaml` | Two blocks. `rmf_fleet:` is the standard EasyFullControl `from_config_files` schema (traits, battery, task capabilities, robots + chargers). `vda5050:` is read by this adapter only: `interface_name`, `update_rate_hz`, `mqtt: {host, port, username, password}`, and per-robot `{manufacturer, serial, transform}`. The `vda5050` identity MUST match the robot-side client adapter so both ends share the same MQTT topics. |
| `maps/nav_graph.yaml` | RMF navigation graph (vertices = waypoints with names / charger / parking flags, lanes = connectivity). Waypoint names become VDA5050 nodeIds. |
| `maps/tb3_world.building.yaml` | Source building map the nav graph is derived from. |
| `launch/fleet_adapter.launch.py` | Launches the `fleet_adapter` node with `config_file` and `nav_graph` arguments (overridable). |

### Tooling

| File | Responsibility |
|---|---|
| `docker/Dockerfile` | Jazzy + RMF + paho-mqtt-cpp build environment for the adapter. |
| `docker/run.sh` | Builds the image (once) and opens an interactive shell with the workspace mounted, host networking, and an isolated `ROS_DOMAIN_ID`. |
| `scripts/dispatch_patrol.py` | Publishes a `patrol` task request to the RMF task API (one or more waypoints, optional rounds). |
| `scripts/cancel_task.py` | Cancels a dispatched task by id. |
| `scripts/mock_mqtt_robot.py` | Simulates a VDA5050 AGV over MQTT (connection / state / order driving / cancelOrder), replacing the whole robot side so the adapter can run without hardware. |
| `scripts/test_dispatch_e2e.py` | End-to-end test: launches the mock robot, dispatches a patrol, and asserts the adapter published an order and the robot reached the target. |

### Tests

| File | Responsibility |
|---|---|
| `test/test_protocol.cpp` | Order/instantActions JSON shape, `ParsedState` parsing, completion logic. |
| `test/test_transform.cpp` | `to_robot` / `to_rmf` round-trip and frame correctness. |
| `test/test_state_machine.cpp` | State transitions and `finished()` firing. |

## Data flow

**Downlink (RMF → AGV).** RMF invokes the navigate callback with one
`Destination` → `RobotStateMachine::on_navigate` → `Vda5050Connector::navigate`
builds a two-node order (current pose → destination, coordinates transformed to
the robot frame) and publishes it on `…/order`.

**Uplink (AGV → RMF).** The connector receives `…/state`, caches it as
`ParsedState`. The update loop calls `get_data` (position + battery, transformed
to the RMF frame) and `RobotAdapter::update`, which feeds RMF and lets the state
machine detect completion (`is_command_completed`) and call `finished()`.

## Build & run

```bash
# from ~/ros2_ws
src/vda5050_fleet_adapter/docker/run.sh            # build image (once) + shell
# inside the container:
colcon build --packages-select vda5050_fleet_adapter
source install/setup.bash
ros2 run rmf_traffic_ros2 rmf_traffic_schedule &
ros2 run rmf_task_ros2 rmf_task_dispatcher &
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py
# dispatch (same container / domain):
python3 src/vda5050_fleet_adapter/scripts/dispatch_patrol.py wp6
```

The robot side keeps running its VDA5050 client adapter, bridge and Nav2; the
broker bridges the two ends. Inspect traffic with
`mosquitto_sub -t 'TB3/v2/ROBOTIS/0001/order'` and `…/state`.

## Tests

Unit tests (pure layers — protocol, transform, state machine):

```bash
colcon test --packages-select vda5050_fleet_adapter
```

End-to-end, no hardware (simulated AGV over MQTT). With the RMF core and the
adapter already running in the container:

```bash
# launches the mock robot, dispatches a patrol, asserts arrival
python3 src/vda5050_fleet_adapter/scripts/test_dispatch_e2e.py --target wp6
```

Or run the simulated robot on its own (e.g. to dispatch manually):

```bash
python3 src/vda5050_fleet_adapter/scripts/mock_mqtt_robot.py
```

## Configuration notes

- **MQTT identity** (`interface_name` / `manufacturer` / `serial`) must match the
  robot-side client adapter exactly, or the two ends will not share topics.
- **Transform** maps the RMF nav-graph frame to the robot map frame; identity
  when the two frames coincide.
- **`responsive_wait`** is enabled per robot only when multiple robots contend
  for space; with a single robot it is left off.

## Multi-robot

1. Add an `is_charger` waypoint per robot in `maps/nav_graph.yaml`.
2. Add the robot under both `rmf_fleet.robots` (charger + `responsive_wait`) and
   `vda5050.robots` (manufacturer / serial / transform) in `config/config.yaml`.

The connector and update loop are already multi-robot: one MQTT connection
serves every robot, keyed by `manufacturer/serial`.
