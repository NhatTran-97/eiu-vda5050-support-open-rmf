# vda5050_client_adapter

ROS 2 adapter node that connects a VDA5050 master control (or Open-RMF fleet adapter) to the robot driver stack. Receives `order` and `instantActions` over MQTT, owns the order and drives it on the robot one node at a time through the `NavigateToNode` action, and publishes robot `state`, `visualization`, `connection`, and `factsheet` back to MQTT.

> See [docs/architecture.md](docs/architecture.md) for module design, the state machine, and sequence diagrams.

## Features

- Implements all six VDA5050 v2.1.0 MQTT topics (`order`, `instantActions`, `state`, `visualization`, `connection`, `factsheet`).
- Full order stitch/update protocol (base/horizon tracking, `newBaseRequest`) and order replacement.
- Single order owner: the adapter sends the driver one `NavigateToNode` goal per released node (order id, node, incoming edge) and applies its result; pause, cancel and blocking actions cancel the goal, a restarted driver gets it again.
- NONE/SOFT/HARD action blocking semantics; built-in instant actions (`startPause`, `stopPause`, `cancelOrder`, `stateRequest`) are executed by the adapter and never wait behind other actions.
- `state` publishes on a timer **and** on every change (position updates throttled); changes within one executor tick are coalesced into one message.
- Single-threaded core: MQTT callbacks only queue work for the ROS executor thread.
- `vda5050.strict_mode` (default `true`) follows VDA5050 2.1 order/action handling; `false` keeps the legacy behavior (see [Strict mode](#strict-mode)).
- Robot driver only executes single node steps and reports telemetry — all VDA5050 protocol complexity is contained here.
- 217 tests, including a wire-compatibility suite that uses `vda5050_fleet_adapter_full_control`'s own message builders and parsers.

## Package Structure

| File | Role |
|:---:|---|
| `src/vda5050_node.cpp` | Main ROS node: owns publishers/subscribers and the `NavigateToNode` client, wires MQTT callbacks to logic managers, executes the route, publishes all outbound VDA5050 messages |
| `src/adapter_state_machine.cpp` | Top-level adapter mode, connectivity state, control-action confirmation, fault handling |
| `src/order_manager.cpp` | VDA5050 order stitch validation, base/horizon tracking, next route step, `newBaseRequest` trigger |
| `src/action_manager.cpp` | Per-action lifecycle with NONE/SOFT/HARD blocking semantics |
| `src/mqtt_client.cpp` | Paho MQTT C++ async client with reconnect and QoS support |
| `include/.../vda5050_types.hpp` | Internal domain model — no external dependencies |
| `include/.../json_converter.hpp` | JSON ↔ internal model (VDA5050 v2.1.0 schema compliant) |
| `include/.../ros_converters.hpp` | ROS 2 messages ↔ internal model (bidirectional) |
| `config/vda5050_params.yaml` | MQTT broker, VDA5050 identity, factsheet, timing |
| `docker/` · `docker-compose.yml` | Build environment and compose stack for the robot side |

## ROS Interface

> All topics and the action are relative to this node's own name (`~/...`), matched 1:1 by `tb3_vda5050_bridge`'s `adapter_ns`-prefixed names on the other side. Client and bridge must be deployed from the same `vda5050_msgs`.

### Subscribed (robot driver → adapter)

| Topic | Type | Purpose |
|:---:|:---:|---|
| `~/agv_position` | `vda5050_msgs/AgvPosition` | Position |
| `~/velocity` | `vda5050_msgs/Velocity` | Velocity |
| `~/battery_state` | `vda5050_msgs/BatteryState` | Battery charge |
| `~/safety_state` | `vda5050_msgs/SafetyState` | eStop / protective field state |
| `~/driver_status` | `vda5050_msgs/DriverStatus` | `session_id` + `driving`. `transient_local`, depth 1. A new `session_id` means the driver restarted: the step goal in flight is sent again. Liveliness lost (the driver offers a lease): step dropped, `driverConnectionError` (FATAL) until it is back |
| `~/operating_mode` | `std_msgs/String` | `AUTOMATIC`/`MANUAL`, from the bridge's manual-override detection. `transient_local`, depth 1; an unknown name keeps the current mode |
| `~/load` | `vda5050_msgs/Load` | Carried load, for `state.loads` |
| `~/action_state_feedback` | `vda5050_msgs/ActionState` | Action progress |
| `~/error` | `vda5050_msgs/Error` | Driver errors |
| `~/distance_since_last_node` | `std_msgs/Float64` | Real driven distance, streamed live for `state.distanceSinceLastNode` |

### Published / action client (adapter → robot driver)

| Name | Type | Purpose |
|:---:|:---:|---|
| `~/navigate_to_node` | `vda5050_msgs/action/NavigateToNode` (client) | One goal per route node: `order_id`, `order_update_id`, `node`, `incoming_edge`. Feedback `edge_entered`; result `REACHED` (with `distance_driven`) / `FAILED` / `DROPPED` / `CANCELED` / `PREEMPTED` |
| `~/action_execute` | `vda5050_msgs/Action` | External action request |
| `~/action_command` | `vda5050_msgs/ActionCommand` | `PAUSE` / `RESUME` / `CANCEL` of one running action |

### Published (local status for on-robot tools such as `robot_local_ui`)

| Topic | Type | Purpose |
|:---:|:---:|---|
| `~/driving` / `~/paused` | `std_msgs/Bool` | Driver's driving flag / adapter's paused flag. `transient_local`, depth 1 |
| `~/node_reached` | `vda5050_msgs/NodeState` | Each reached node with `distance_driven` |
| `~/error` | `vda5050_msgs/Error` | `navigationError` FATAL of a failed step (`orderId`, `nodeId`). The adapter also subscribes to `~/error` for driver errors and ignores a `navigationError` naming an order that is no longer active, so its own copy is not read back |

Orders reach the robot only through MQTT; the bridge has no order input.

Step handling:

| Event | Adapter |
|---|---|
| Released node not yet reached, driving allowed | Sends its goal; a newer order's step preempts the goal in flight |
| Feedback `edge_entered` | Incoming edge entered, its actions start |
| `REACHED` (result code `SUCCEEDED`) | Edge completed, node reached with `distance_driven`, node actions start, next step |
| `FAILED` | `navigationError` (FATAL, `orderId` + `nodeId`) published once, then the order is dropped and the error cleared |
| `DROPPED` (local cancel on the robot, `initPosition`) | Order dropped, no error |
| `startPause` / `cancelOrder` / strict mode HARD or SOFT action running | Goal cancelled; `paused` / cancel completion wait for its result |
| Goal ended without `SUCCEEDED`/`ABORTED` (driver shut down) | No progress; the step is sent again |
| `~/driver_status` liveliness lost | Step dropped, `driving` false, `driverConnectionError` (FATAL); pause / cancel complete; no new step until the driver is back, then the error is cleared and the step sent again |
| Adapter start | Every goal left on the driver by a previous adapter process is cancelled before the first step |

## Configuration

Config file: [`config/vda5050_params.yaml`](config/vda5050_params.yaml)

| Parameter | Default | Description |
|:---:|:---:|---|
| `mqtt.broker_url` | `tcp://localhost:1883` | MQTT broker address |
| `mqtt.client_id` | `""` | Must be unique per AGV connected to the broker — a duplicate disconnects the other one. Empty: `vda5050_<interface_name>_<manufacturer>_<serial_number>` |
| `mqtt.username` / `mqtt.password` | `""` | Broker auth, if required |
| `vda5050.interface_name` | `TB3` | Must match fleet adapter |
| `vda5050.manufacturer` | `ROBOTIS` | Must match fleet adapter |
| `vda5050.serial_number` | `0001` | Must match fleet adapter |
| `vda5050.state_publish_interval` | `30.0` | Timer interval (sec) for `state` publishes |
| `vda5050.visualization_interval` | `1.0` | Timer interval (sec) for `visualization` publishes |
| `vda5050.position_publish_min_interval` | `1.0` | Min seconds between extra `state` publishes triggered by a fresh position update |
| `vda5050.hard_action_pause_timeout` | `30.0` | Default mode: max seconds a HARD-blocking action waits for another action to confirm it paused before it's failed instead of waiting forever |
| `vda5050.event_loop_period` | `0.01` | Executor tick that runs queued MQTT work and publishes a changed `state` (at most once per tick), in (0, 1] s |
| `vda5050.new_base_request_min_base_nodes` | `2` | `newBaseRequest` is raised when fewer released nodes remain and a horizon exists |
| `vda5050.max_finished_instant_actions` | `50` | Finished/failed instant actions kept in `actionStates` (oldest dropped first; `0` = keep all until the next order) |
| `vda5050.strict_mode` | `true` | VDA5050 2.1 order/action handling; `false` = legacy, see below |
| `vda5050.driver_status_max_lease` | `10.0` | Liveliness lease (s) requested on `~/driver_status`; the driver's offered lease must not exceed it (else no match, logged). `0` = no driver-lost detection. A finite value is needed for Fast DDS to report liveliness |
| `factsheet.type_specification.*`, `factsheet.physical_parameters.*`, `factsheet.supported_action_types` | — | AGV type, kinematics, load capacity, speed/accel/dimensions, and supported VDA5050 actions — published once as the retained `factsheet` message. See the yaml file for the full field list. |

## Build & Run

```bash
# Using Docker Compose (recommended on TurtleBot3)
cd src/vda5050_client_adapter
docker compose up -d --build

# Or build from source (Humble/Jazzy)
sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev nlohmann-json3-dev
colcon build --packages-select vda5050_msgs vda5050_client_adapter
source install/setup.bash
ros2 launch vda5050_client_adapter vda5050_adapter.launch.py
# Non-empty arguments override the params file: broker_url, client_id, manufacturer, serial_number
ros2 launch vda5050_client_adapter vda5050_adapter.launch.py broker_url:=tcp://192.168.1.10:1883 serial_number:=0002
```

Invalid timing/limit parameters stop the node at startup with the offending key in the error.

`nlohmann-json3-dev` is optional but recommended: without it CMake fetches
nlohmann/json from GitHub at configure time, which needs network access and
breaks offline/air-gapped builds.

## Strict mode

`vda5050.strict_mode: true` (default) follows VDA5050 2.1 §6.6/§6.12; `false` selects the legacy
handling:

| | Legacy (`false`) | Strict (`true`, default) |
|---|---|---|
| New `orderId` while an order is active | Replaces it (first node must be the last traversed node) | `orderError`, order kept |
| Lower `orderUpdateId` | `orderError` | `orderUpdateError` |
| Node/edge structure (sequence ids, edge endpoints, base/horizon split, unique `actionId`) | Not checked | `validationError` |
| Update stitched at the horizon end | Accepted | `orderUpdateError` |
| `orderId`/`orderUpdateId` after `cancelOrder` | Cleared | Kept |
| HARD action | Pauses running actions, then runs | Waits for running actions to end; later actions wait for it |
| `state.driving` | Masked while paused, action-blocked or faulted | Driver value |
| Running HARD/SOFT action | Route keeps going | Route waits (step goal cancelled) until it ends |

Both modes: an order or instantActions message that fails to parse (bad JSON, unknown enum such as
`"blockingType": "hard"`) is reported as `validationError` and not executed; an order repeating the
current `orderId` and `orderUpdateId` is ignored (VDA5050 6.6.4.3, a master resend); `factsheetRequest`
republishes the factsheet.

## Testing

```bash
colcon test --packages-select vda5050_client_adapter
colcon test-result --verbose

# Live wire-compatibility cases need a private broker
mosquitto -p 18830 &
VDA5050_TEST_BROKER=tcp://127.0.0.1:18830 ROS_DOMAIN_ID=77 ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
  build/vda5050_client_adapter/test_full_control_compat
```

| Suite | Tests | Coverage |
|:---:|:---:|---|
| `test_adapter_state_machine` | 5 | Mode transitions, confirmations, fault/shutdown, pending-action supersede, driving flag |
| `test_order_manager` | 59 | Accept, stitch, newBaseRequest, cancel, reject, order replacement, next step and progress, strict mode |
| `test_action_manager` | 39 | NONE/SOFT/HARD blocking, control actions, sequential HARD, pause/resume/cancel, HARD-wait timeout, instant action cap |
| `test_converters` | 50 | JSON round-trips, schema compliance, strict enums, ROS↔internal |
| `test_full_control_compat` | 64 | `vda5050_fleet_adapter_full_control` builders/parsers against the client; live node + fake `NavigateToNode` server + MQTT in both modes: route steps, update without resend, preemption, cancel/pause/resume, blocking actions, failed/dropped/abandoned steps, driver restart, lost driver, stale goals at start, local status topics, factsheetRequest (54 cases need `VDA5050_TEST_BROKER`) |

`test_full_control_compat` compiles sources from `../vda5050_fleet_adapter_full_control`
(CMake cache `VDA5050_FULL_CONTROL_SOURCE_DIR`); it is skipped when they are absent.

## Related

- [Root README — system overview](../README.md)
- [Detailed Architecture](docs/architecture.md)
- [MQTT Test Guide](docs/mqtt_test_guide.md)
- [Docker Guide](docs/docker_guide.md)
- [TB3 VDA5050 Bridge](../tb3_vda5050_bridge/README.md)
