# tb3_vda5050_bridge

ROS 2 bridge node that connects `vda5050_client_adapter` to the TurtleBot3 / Nav2 stack. Executes the adapter's `NavigateToNode` steps (one route node each) as Nav2 `NavigateToPose` goals and feeds odometry, battery and driver status back to the adapter. The order itself lives only in the adapter.

> See [docs/architecture.md](docs/architecture.md) for module design, the state machine, and sequence diagrams.

## Features

- `NavigateToNode` action server: drives to one node per goal, reports the incoming edge as entered once Nav2 accepts, and ends with `REACHED` (with the distance driven), `FAILED`, `DROPPED`, `CANCELED` or `PREEMPTED`.
- A node the robot already stands on, or a node without position once the pose is valid, is reached without Nav2.
- Feeds position, velocity, battery, driving and real driven distance back to the adapter; `driver_status` carries a random session id per process so the adapter resends a step lost in a restart.
- Detects manual joystick/keyboard takeover via `twist_mux`/`/diagnostics` and reports it as `operating_mode`.
- Survives out-of-order startup (bridge before Nav2) and retries a failed Nav2 goal until `nav2_dispatch_timeout_sec` instead of failing the step on the first hiccup.
- `initPosition` re-localization via AMCL, refused while a goal drives from a valid pose.

See [docs/architecture.md](docs/architecture.md) for how each of these is implemented.

## Package Structure

| File | Role |
|:---:|---|
| `src/bridge_node.cpp` | `NavigateToNode` server, Nav2 client, telemetry publishers, instant actions. |
| `src/main.cpp` | Node executable. |
| `src/odom_distance_tracker.cpp` | Accumulates real driven distance between nodes from consecutive `/odom` positions. |
| `config/bridge_params.yaml` | ROS parameters (adapter namespace, topic names, Nav2 action name). |

## ROS Interface

> Topic prefix is parameterized by `adapter_ns` (default: `/vda5050_client_adapter`).

### Subscribed

| Topic | Type | Purpose |
|:---:|:---:|---|
| `${odom_topic}` | `nav_msgs/Odometry` | Robot position and velocity |
| `${amcl_pose_topic}` | `geometry_msgs/PoseWithCovarianceStamped` | AMCL localization pose |
| `${battery_topic}` | `sensor_msgs/BatteryState` | Battery charge |
| `${diagnostics_topic}` | `diagnostic_msgs/DiagnosticArray` | `twist_mux` arbitration, for manual-override detection |
| `${adapter_ns}/navigate_to_node` | `vda5050_msgs/action/NavigateToNode` (server) | One step: drive to `node` over `incoming_edge`. A new goal preempts the active one (`PREEMPTED`); a cancel request stops Nav2 (`CANCELED`) |
| `${adapter_ns}/action_execute` | `vda5050_msgs/Action` | External action request |
| `${adapter_ns}/action_command` | `vda5050_msgs/ActionCommand` | Pause / resume / cancel of one action; ignored (actions here complete at once) |
| `${adapter_ns}/action_cancel` | `std_msgs/String` | Local tools (`robot_local_ui`): `cancel:*` stops the active step (`DROPPED`, the adapter drops the order); anything else is ignored |

### Published

| Topic | Type | Purpose |
|:---:|:---:|---|
| `${adapter_ns}/agv_position` | `vda5050_msgs/AgvPosition` | Robot position |
| `${adapter_ns}/velocity` | `vda5050_msgs/Velocity` | Robot velocity, at most every `odom_publish_min_interval_sec` |
| `${adapter_ns}/battery_state` | `vda5050_msgs/BatteryState` | Battery feedback — an unusable `/battery_state` reading (no percentage or voltage) republishes the last known-good value instead of fabricating one |
| `${adapter_ns}/driver_status` | `vda5050_msgs/DriverStatus` | `session_id` (random per process) and `driving` (a Nav2 goal is active). `transient_local`, depth 1, manual-by-topic liveliness with lease `driver_status_lease_sec`; republished every lease / 3 |
| `${adapter_ns}/operating_mode` | `std_msgs/String` | `AUTOMATIC` or `MANUAL`, from `twist_mux` arbitration. `transient_local`, depth 1 |
| `${adapter_ns}/action_state_feedback` | `vda5050_msgs/ActionState` | Action progress |
| `${adapter_ns}/distance_since_last_node` | `std_msgs/Float64` | Real driven distance between nodes, at most every `odom_publish_min_interval_sec` |
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
| `nav2_dispatch_timeout_sec` | `120.0` | Max time to reach a step's node while Nav2 is unavailable, rejects or ends goals early; then the step `FAILED`. Not spent while in `MANUAL` |
| `initial_pose_topic` | `/initialpose` | Where `initPosition` publishes AMCL's new pose. Refused (`FAILED`) while a Nav2 goal is active and the pose is valid; otherwise the active step ends `DROPPED` first |
| `supported_action_types` | `[]` | VDA5050 action types actually implemented (e.g. `initPosition`) |
| `amcl_pose_timeout_sec` | `10.0` | Max age of the last AMCL pose before it stops being trusted — unless the robot hasn't moved since, see `pose_stale_move_tolerance_m` |
| `pose_stale_move_tolerance_m` | `0.15` | Only checked once the robot has driven since the last AMCL confirmation — a stale AMCL pose is still trusted as long as odometry shows no more than this much movement since |
| `speed_limit_topic` | `/speed_limit` | Nav2's speed-override input; an edge's `maxSpeed` is applied here before dispatch |
| `nav2_retry_period_sec` | `2.0` | Period of the dispatch retry while Nav2 is unavailable, rejects or ends a goal early |
| `odom_publish_min_interval_sec` | `0.1` | Min interval of `velocity` / `distance_since_last_node` publishes (`0` = every odometry message) |
| `driver_status_lease_sec` | `1.0` | Liveliness lease of `driver_status`; the adapter treats the driver as lost once it expires. Must not exceed the adapter's `vda5050.driver_status_max_lease` (10 s) |
| `default_allowed_deviation_xy` | `0.5` | Node tolerance (m) when the order sets no `allowedDeviationXY` |
| `unconstrained_theta_rad` | `3.0` | `allowedDeviationTheta` at or above this ignores the node heading |
| `battery_voltage_full` / `battery_voltage_empty` | `12.6` / `9.0` | Voltage range used when the battery reports no percentage |
| `initial_pose_covariance_xy` / `initial_pose_covariance_yaw` | `0.25` / `0.0685` | Covariance of the `initPosition` pose |
| `diagnostics_topic` | `/diagnostics` | `twist_mux` diagnostics |
| `twist_mux_status_name` | `twist_mux: Twist mux status` | Diagnostic status read for manual override |
| `navigation_velocity_source` | `navigation` | `twist_mux` input of Nav2; any other unmasked input means `MANUAL` |

Invalid values stop the node at startup with the offending key in the error.

## Build & Run

```bash
# Build (on TurtleBot3 or cross-compiled)
colcon build --packages-select vda5050_msgs tb3_vda5050_bridge
source install/setup.bash

# Run (use_mock_load:=false on a robot with a real load sensor)
ros2 launch tb3_vda5050_bridge bridge.launch.py
ros2 launch tb3_vda5050_bridge bridge.launch.py use_mock_load:=false
```

Start order does not matter: the adapter waits for the step server, and the bridge retries until Nav2 is up. The adapter and the bridge must be built from the same `vda5050_msgs`.

## Testing without hardware

`mock/mock_load_publisher.py` stands in for a load sensor that doesn't exist
yet — it republishes a fixed `vda5050_msgs/Load` onto
`vda5050_client_adapter/load` (relative to the launch namespace, `--topic` to change it) every second, so `state.loads` reaches the
fleet adapter and the UI over the real MQTT `state` message, proving the
pipeline end to end. Swap it for a real sensor node later; nothing
downstream (adapter, MQTT, fleet adapter, UI) needs to change.

```bash
ros2 run tb3_vda5050_bridge mock_load_publisher.py \
    --load-id box-01 --load-type box --weight 20
```

## Tests

```bash
colcon test --packages-select tb3_vda5050_bridge
# test_system_e2e needs vda5050_client_adapter, the vda5050_fleet_adapter_full_control sources and a broker
VDA5050_TEST_BROKER=tcp://127.0.0.1:18830 ROS_DOMAIN_ID=77 ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
  build/tb3_vda5050_bridge/test_system_e2e
```

| Suite | Tests | Coverage |
|:---:|:---:|---|
| `test_odom_distance_tracker` | 6 | Driven distance |
| `test_bridge_node` | 22 | Node against a fake Nav2 and a fake adapter (`NavigateToNode` client): reach with distance, node under the robot, position-less node, speed limit, preemption, cancel, local cancel, retries and manual override, session id, liveliness lease and heartbeat, actions, telemetry |
| `test_system_e2e` | 10 | Fleet adapter (MQTT, `full_control` builders) → client adapter → bridge → fake Nav2 in both client modes, including a bridge restart mid-route (step resent) and a bridge gone until it is back (`driverConnectionError`) |

## Related

- [Root README — system overview](../README.md)
- [Detailed Architecture](docs/architecture.md)
- [VDA5050 Client Adapter](../vda5050_client_adapter/README.md)
