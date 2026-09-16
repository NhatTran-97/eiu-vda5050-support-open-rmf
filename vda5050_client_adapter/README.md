# vda5050_client_adapter

ROS 2 adapter node that connects a VDA5050 master control (or Open-RMF fleet adapter) to the robot driver stack. Receives `order` and `instantActions` over MQTT, exposes them as ROS 2 topics, and publishes robot `state`, `visualization`, `connection`, and `factsheet` back to MQTT.

> See [docs/architecture.md](docs/architecture.md) for module design, the state machine, and sequence diagrams.

## Features

- Implements all six VDA5050 v2.1.0 MQTT topics (`order`, `instantActions`, `state`, `visualization`, `connection`, `factsheet`).
- Full order stitch/update protocol (base/horizon tracking, `newBaseRequest`) and order replacement with stale-echo absorption.
- NONE/SOFT/HARD action blocking semantics; built-in instant actions (`startPause`, `stopPause`, `cancelOrder`, `stateRequest`).
- `state` publishes on a timer **and** immediately on a fresh position update (throttled), so Master Control sees live movement instead of a once-per-timer jump.
- Robot driver only needs to speak plain ROS 2 topics — all VDA5050 protocol complexity is contained here.
- 116 unit tests covering the state machine, order manager, action manager, and JSON/ROS converters.

## Package Structure

| File | Role |
|---|---|
| `src/vda5050_node.cpp` | Main ROS node: owns publishers/subscribers, wires MQTT callbacks to logic managers, publishes all outbound VDA5050 messages |
| `src/adapter_state_machine.cpp` | Top-level adapter mode, connectivity state, control-action confirmation, fault handling |
| `src/order_manager.cpp` | VDA5050 order stitch validation, base/horizon tracking, `newBaseRequest` trigger |
| `src/action_manager.cpp` | Per-action lifecycle with NONE/SOFT/HARD blocking semantics |
| `src/mqtt_client.cpp` | Paho MQTT C++ async client with reconnect and QoS support |
| `include/.../vda5050_types.hpp` | Internal domain model — no external dependencies |
| `include/.../json_converter.hpp` | JSON ↔ internal model (VDA5050 v2.1.0 schema compliant) |
| `include/.../ros_converters.hpp` | ROS 2 messages ↔ internal model (bidirectional) |
| `config/vda5050_params.yaml` | MQTT broker, VDA5050 identity, factsheet, timing |
| `docker/` · `docker-compose.yml` | Build environment and compose stack for the robot side |

## ROS Interface

> All topics are relative to this node's own name (`~/...`), matched 1:1 by `tb3_vda5050_bridge`'s `adapter_ns`-prefixed topics on the other side.

### Subscribed (robot driver → adapter)

| Topic | Type | Purpose |
|---|---|---|
| `~/agv_position` | `vda5050_msgs/AgvPosition` | Position |
| `~/velocity` | `vda5050_msgs/Velocity` | Velocity |
| `~/battery_state` | `vda5050_msgs/BatteryState` | Battery charge |
| `~/safety_state` | `vda5050_msgs/SafetyState` | eStop / protective field state |
| `~/driving` / `~/paused` | `std_msgs/Bool` | Motion state. `transient_local`, depth 1 — gets the retained value immediately even if this node (re)started after the bridge |
| `~/operating_mode` | `std_msgs/String` | `AUTOMATIC`/`MANUAL`, from the bridge's manual-override detection |
| `~/load` | `vda5050_msgs/Load` | Carried load, for `state.loads` |
| `~/node_reached` / `~/edge_entered` / `~/edge_completed` | `vda5050_msgs/NodeState` / `EdgeState` | Traversal feedback |
| `~/action_state_feedback` | `vda5050_msgs/ActionState` | Action progress |
| `~/error` | `vda5050_msgs/Error` | Bridge/navigation errors |
| `~/order_dropped` | `std_msgs/String` | Bridge gave up on an order outside `cancelOrder` — adapter clears its own tracking to match |
| `~/distance_since_last_node` | `std_msgs/Float64` | Real driven distance, streamed live for `state.distanceSinceLastNode` |

### Published (adapter → robot driver)

| Topic | Type | Purpose |
|---|---|---|
| `~/order` | `vda5050_msgs/Order` | Validated order to robot driver. `transient_local` — a (re)starting bridge gets the current order immediately |
| `~/action_execute` | `vda5050_msgs/Action` | External action request |
| `~/action_cancel` | `std_msgs/String` | `pause:*` / `resume:*` / `cancel:*` signal |

## Configuration

Config file: [`config/vda5050_params.yaml`](config/vda5050_params.yaml)

| Parameter | Default | Description |
|---|---|---|
| `mqtt.broker_url` | `tcp://localhost:1883` | MQTT broker address |
| `mqtt.client_id` | `vda5050_client_adapter` | Must be unique per AGV connected to the broker — a duplicate disconnects the other one |
| `mqtt.username` / `mqtt.password` | `""` | Broker auth, if required |
| `vda5050.interface_name` | `TB3` | Must match fleet adapter |
| `vda5050.manufacturer` | `ROBOTIS` | Must match fleet adapter |
| `vda5050.serial_number` | `0001` | Must match fleet adapter |
| `vda5050.state_publish_interval` | `30.0` | Timer interval (sec) for `state` publishes |
| `vda5050.visualization_interval` | `1.0` | Timer interval (sec) for `visualization` publishes |
| `vda5050.position_publish_min_interval` | `1.0` | Min seconds between extra `state` publishes triggered by a fresh position update |
| `vda5050.hard_action_pause_timeout` | `30.0` | Max seconds a HARD-blocking action waits for another action to confirm it paused before it's failed instead of waiting forever |
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
ros2 launch vda5050_client_adapter vda5050_client_adapter.launch.py
```

`nlohmann-json3-dev` is optional but recommended: without it CMake fetches
nlohmann/json from GitHub at configure time, which needs network access and
breaks offline/air-gapped builds.

## Testing

```bash
# 116 unit tests (all pass)
colcon test --packages-select vda5050_client_adapter
colcon test-result --verbose
```

| Suite | Tests | Coverage |
|---|---|---|
| `test_adapter_state_machine` | 4 | Mode transitions, confirmations, fault/shutdown, pending-action supersede |
| `test_order_manager` | 36 | Accept, stitch, newBaseRequest, cancel, reject, zone_set_id clear, edge_entered ordering, order replacement, stale-echo absorption |
| `test_action_manager` | 30 | NONE/SOFT/HARD blocking, pause/resume/cancel, status transition guard, HARD-wait timeout |
| `test_converters` | 46 | JSON round-trips, schema compliance, ROS↔internal |

## Related

- [Root README — system overview](../README.md)
- [Detailed Architecture](docs/architecture.md)
- [MQTT Test Guide](docs/mqtt_test_guide.md)
- [Docker Guide](docs/docker_guide.md)
- [TB3 VDA5050 Bridge](../tb3_vda5050_bridge/README.md)
