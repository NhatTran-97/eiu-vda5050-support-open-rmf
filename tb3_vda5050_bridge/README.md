# tb3_vda5050_bridge

ROS 2 bridge node that connects `vda5050_client_adapter` to the TurtleBot3 / Nav2 stack. Converts VDA5050 orders into `NavigateToPose` goals and feeds odometry, battery, and traversal events back to the adapter.

> See [docs/architecture.md](docs/architecture.md) for module design, the state machine, and sequence diagrams.

## Features

- Executes VDA5050 orders node-by-node as Nav2 `NavigateToPose` goals, including action-only nodes and in-place order updates (horizon reshaping).
- Feeds position, velocity, battery, driving/paused state, and real driven distance back to the adapter.
- Detects manual joystick/keyboard takeover via `twist_mux`/`/diagnostics` and reports it as `operating_mode`.
- Survives out-of-order startup (bridge before Nav2), goal preemption races, and process restarts (order progress persisted to disk).
- Retries a failed Nav2 goal instead of failing the order on the first hiccup.
- `initPosition` re-localization via AMCL, refused while a goal drives from a valid pose.

See [Startup & Robustness](#startup--robustness) below for the operational detail, and [docs/architecture.md](docs/architecture.md) for how each of these is implemented.

## Package Structure

| File | Role |
|:---:|---|
| `src/bridge_node.cpp` | ROS orchestration: owns all publishers/subscribers and the Nav2 action client. Translates between the ROS interface and the session/state-machine. |
| `src/order_session.cpp` | `plan_next_work()` traversal algorithm: released action-only nodes are consumed immediately; navigable nodes are sent to Nav2; unreleased nodes trigger WAITING_FOR_RELEASE. |
| `src/bridge_state_machine.cpp` | Centralizes mode transitions and derives `driving` / `paused` flags. Prevents contradictory states. |
| `src/odom_distance_tracker.cpp` | Accumulates real driven distance between nodes from consecutive `/odom` positions. |
| `config/bridge_params.yaml` | ROS parameters (adapter namespace, topic names, Nav2 action name). |

## Startup & Robustness

The bridge is designed so that **startup order does not matter**, goal
replacement doesn't race, restarts don't replay finished work, and a
transient Nav2 failure doesn't fail the whole order. See
[docs/architecture.md §6 and §6b](docs/architecture.md#6-stale-callback-protection)
for exactly how.

## ROS Interface

> Topic prefix is parameterized by `adapter_ns` (default: `/vda5050_client_adapter`).

### Subscribed

| Topic | Type | Purpose |
|:---:|:---:|---|
| `${odom_topic}` | `nav_msgs/Odometry` | Robot position and velocity |
| `${amcl_pose_topic}` | `geometry_msgs/PoseWithCovarianceStamped` | AMCL localization pose |
| `${battery_topic}` | `sensor_msgs/BatteryState` | Battery charge |
| `/diagnostics` | `diagnostic_msgs/DiagnosticArray` | `twist_mux` arbitration, for manual-override detection |
| `${adapter_ns}/order` | `vda5050_msgs/Order` | Active order from adapter |
| `${adapter_ns}/action_cancel` | `std_msgs/String` | `pause:*` · `resume:*` · `cancel:*` |
| `${adapter_ns}/action_execute` | `vda5050_msgs/Action` | External action request |

### Published

| Topic | Type | Purpose |
|:---:|:---:|---|
| `${adapter_ns}/agv_position` | `vda5050_msgs/AgvPosition` | Robot position |
| `${adapter_ns}/velocity` | `vda5050_msgs/Velocity` | Robot velocity |
| `${adapter_ns}/battery_state` | `vda5050_msgs/BatteryState` | Battery feedback — an unusable `/battery_state` reading (no percentage or voltage) republishes the last known-good value instead of fabricating one |
| `${adapter_ns}/driving` | `std_msgs/Bool` | Derived from state machine. `transient_local`, depth 1 |
| `${adapter_ns}/paused` | `std_msgs/Bool` | Derived from state machine. `transient_local`, depth 1 |
| `${adapter_ns}/operating_mode` | `std_msgs/String` | `AUTOMATIC` or `MANUAL`, from `twist_mux` arbitration |
| `${adapter_ns}/node_reached` | `vda5050_msgs/NodeState` | Traversal event |
| `${adapter_ns}/edge_entered` | `vda5050_msgs/EdgeState` | Edge activation event |
| `${adapter_ns}/edge_completed` | `vda5050_msgs/EdgeState` | Edge completion event |
| `${adapter_ns}/action_state_feedback` | `vda5050_msgs/ActionState` | Action progress |
| `${adapter_ns}/error` | `vda5050_msgs/Error` | Navigation or bridge errors |
| `${adapter_ns}/order_dropped` | `std_msgs/String` | `orderId` this bridge gave up on outside `cancelOrder` |
| `${adapter_ns}/distance_since_last_node` | `std_msgs/Float64` | Real driven distance, streamed live between nodes |
| `${initial_pose_topic}` | `geometry_msgs/PoseWithCovarianceStamped` | AMCL re-localization from `initPosition` (not adapter-namespaced) |
| `${speed_limit_topic}` | `nav2_msgs/SpeedLimit` | An edge's `maxSpeed` applied to Nav2 (not adapter-namespaced) |

## Configuration

Config file: [`config/bridge_params.yaml`](config/bridge_params.yaml)

| Parameter | Default | Description |
|:---:|:---:|---|
| `adapter_ns` | `/vda5050_client_adapter` | Adapter topic prefix |
| `odom_topic` | `/odom` | Odometry input |
| `amcl_pose_topic` | `/amcl_pose` | AMCL pose input |
| `battery_topic` | `/battery_state` | Battery input |
| `nav2_action_name` | `navigate_to_pose` | Nav2 action server name |
| `map_id` | `map` | Default map frame reported in `AgvPosition` |
| `nav2_frame_id` | `map` | Nav2 global frame used for goal poses |
| `position_covariance_threshold` | `0.5` | Threshold for `position_initialized` flag |
| `order_state_path` | `$HOME/.ros/tb3_vda5050_bridge_order_state.txt` | Order progress persisted across restarts |
| `nav2_dispatch_timeout_sec` | `120.0` | Max wait for Nav2 before failing a stuck order |
| `initial_pose_topic` | `/initialpose` | Where `initPosition` publishes AMCL's new pose. Refused (`FAILED`) while a goal is active and the pose is valid; if the pose is lost the goal is stopped first |
| `supported_action_types` | `[]` | VDA5050 action types actually implemented (e.g. `initPosition`) |
| `amcl_pose_timeout_sec` | `10.0` | Max age of the last AMCL pose before it stops being trusted — unless the robot hasn't moved since, see `pose_stale_move_tolerance_m` |
| `pose_stale_move_tolerance_m` | `0.15` | Only checked once the robot has driven since the last AMCL confirmation — a stale AMCL pose is still trusted as long as odometry shows no more than this much movement since |
| `speed_limit_topic` | `/speed_limit` | Nav2's speed-override input; an edge's `maxSpeed` is applied here before dispatch |

## Build & Run

```bash
# Build (on TurtleBot3 or cross-compiled)
colcon build --packages-select vda5050_msgs tb3_vda5050_bridge
source install/setup.bash

# Run
ros2 launch tb3_vda5050_bridge bridge.launch.py
```

Make sure `vda5050_client_adapter` and Nav2 are running before starting the bridge.

## Testing without hardware

`mock/mock_load_publisher.py` stands in for a load sensor that doesn't exist
yet — it republishes a fixed `vda5050_msgs/Load` onto
`/vda5050_client_adapter/load` every second, so `state.loads` reaches the
fleet adapter and the UI over the real MQTT `state` message, proving the
pipeline end to end. Swap it for a real sensor node later; nothing
downstream (adapter, MQTT, fleet adapter, UI) needs to change.

```bash
ros2 run tb3_vda5050_bridge mock_load_publisher.py \
    --load-id box-01 --load-type box --weight 20
```

## Related

- [Root README — system overview](../README.md)
- [Detailed Architecture](docs/architecture.md)
- [VDA5050 Client Adapter](../vda5050_client_adapter/README.md)
