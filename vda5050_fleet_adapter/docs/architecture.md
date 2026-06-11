# vda5050_fleet_adapter Architecture

This document describes the current architecture of the `vda5050_fleet_adapter`
package. The package connects Open-RMF fleet scheduling to a VDA5050-compatible
robot interface over MQTT.

## Scope

`vda5050_fleet_adapter` is the northbound RMF fleet adapter for one VDA5050 robot.
It registers a fleet and robot with Open-RMF, receives navigation plans from RMF,
converts each path into a VDA5050 `order`, publishes that order to MQTT, and
updates RMF with robot position and path progress from VDA5050 `state` messages.

The package does not drive the robot directly. A robot-side component, such as
`vda5050_client_adapter` plus `tb3_vda5050_bridge`, is expected to subscribe to
the MQTT order topic, execute the motion, and publish VDA5050 state back.

## Runtime Architecture

```text
Open-RMF task dispatcher
        |
        | RMF task / route assignment
        v
rmf_fleet_adapter::agv::Adapter
        |
        | RobotCommandHandle callbacks
        v
vda5050_fleet_adapter
        |
        | MQTT VDA5050 order
        v
MQTT broker
        |
        | order / instantActions / state
        v
VDA5050 robot-side adapter or bridge
        |
        | robot command / feedback
        v
Robot navigation stack
```

The adapter is intentionally thin:

- Open-RMF owns task allocation, schedule negotiation, route planning, and battery
  task planning.
- `FleetAdapter` owns RMF fleet registration, graph loading, vehicle traits, and
  robot registration.
- `RobotCommandHandle` owns the translation between RMF path callbacks and
  VDA5050 MQTT messages.
- The robot-side adapter owns actual motion execution and robot state generation.

## Main Components

### `main.cpp`

`src/main.cpp` is the executable entry point. It:

- initializes ROS 2 with `rclcpp::init()`;
- declares and reads parameters into `FleetConfig`;
- validates that `nav_graph_path` is provided;
- creates `FleetAdapter`;
- calls `FleetAdapter::run()`.

Important parameters include:

- `fleet_name`
- `nav_graph_path`
- `rmf_server_uri`
- `mqtt_broker_url`
- `interface_name`
- `manufacturer`
- `serial_number`
- `robot_start_waypoint`
- `map_name`
- vehicle traits
- battery and power model values

### `FleetConfig`

`FleetConfig` is defined in
`include/vda5050_fleet_adapter/fleet_adapter.hpp`. It is the central runtime
configuration object. The adapter keeps robot identity, MQTT identity, RMF graph
settings, vehicle traits, and battery model values outside the implementation so
the C++ code is not tied to a single robot vendor.

The `map_name` value is especially important. It must match:

- the level name in `maps/nav_graph.yaml`;
- the `mapId` written into VDA5050 order node positions;
- the `map_id` reported by the robot in VDA5050 `agvPosition`.

If these values do not match, RMF can reject position updates.

### `FleetAdapter`

`FleetAdapter` is defined in `fleet_adapter.hpp` and implemented in
`src/fleet_adapter.cpp`. It wraps RMF fleet setup.

Responsibilities:

- create `rmf_fleet_adapter::agv::Adapter`;
- load the navigation graph from `nav_graph.yaml`;
- create RMF `VehicleTraits`;
- register the fleet with RMF using `add_fleet()`;
- configure battery, mechanical, ambient, and tool power models;
- find the robot start waypoint by name;
- find the first charger waypoint in the graph;
- register the robot and attach a `RobotUpdateHandle`;
- start and stop the RMF adapter.

The graph is loaded from Traffic Editor style YAML:

```yaml
levels:
  tb3_world:
    vertices:
      - [x, y, {name: initial_wp}]
    lanes:
      - [0, 1, {}]
```

The loader reads:

- vertex coordinates as RMF waypoints;
- `name` as a graph key for task dispatch;
- `is_charger` as the charger waypoint marker;
- `is_parking_spot` as an RMF holding point;
- lanes as directed RMF graph lanes.

If graph loading fails, the code falls back to a small hardcoded test graph.

### `RobotCommandHandle`

`RobotCommandHandle` is defined in
`include/vda5050_fleet_adapter/robot_command_handle.hpp` and implemented in
`src/robot_command_handle.cpp`.

It inherits from:

- `rmf_fleet_adapter::agv::RobotCommandHandle`
- `mqtt::callback`

Responsibilities:

- connect to the MQTT broker;
- subscribe to the VDA5050 `state` topic;
- receive RMF path requests through `follow_new_path()`;
- convert RMF waypoints into a VDA5050 `order`;
- publish orders to the VDA5050 `order` topic;
- process VDA5050 `state` messages;
- update RMF robot position;
- notify RMF when waypoints are reached;
- notify RMF when the path is finished.

The MQTT topic prefix is:

```text
{interface_name}/v2/{manufacturer}/{serial_number}
```

For example, with the default config:

```text
TB3/v2/ROBOTIS/0001/order
TB3/v2/ROBOTIS/0001/instantActions
TB3/v2/ROBOTIS/0001/state
```

## Navigation Flow

1. A task is submitted to Open-RMF, for example a patrol task.
2. RMF plans a route using the loaded navigation graph.
3. RMF calls `RobotCommandHandle::follow_new_path()` with a sequence of planned
   waypoints.
4. `RobotCommandHandle` creates a new VDA5050 `orderId`.
5. Each RMF waypoint becomes a VDA5050 node:

   ```text
   nodeId: n0, n1, n2, ...
   sequenceId: 0, 2, 4, ...
   nodePosition: x, y, theta, mapId
   ```

6. Each segment between two nodes becomes a VDA5050 edge:

   ```text
   edgeId: e0, e1, e2, ...
   sequenceId: 1, 3, 5, ...
   startNodeId -> endNodeId
   ```

7. The complete order is published to the MQTT `order` topic.
8. The robot-side adapter executes the order and publishes VDA5050 `state`.
9. `RobotCommandHandle::_on_state()` reads `agvPosition` and calls
   `RobotUpdateHandle::update_position()`.
10. When `lastNodeId` changes, the adapter calls RMF's arrival estimator for the
    corresponding waypoint index.
11. When `nodeStates` is empty and `driving` is false, the path is marked
    complete and RMF may assign the next path.

The adapter only accepts progress for the current `orderId`. This avoids stale
state from an older order accidentally completing a newly issued path.

## Stop And Replanning Behavior

`RobotCommandHandle::stop()` currently logs the stop request but does not publish
a VDA5050 `cancelOrder`.

The reason is that RMF may call `stop()` immediately before sending a new path.
Publishing a cancel action at that moment can create a race where the robot is
still finishing the cancel action while the replacement order arrives. The code
therefore relies on a replacement VDA5050 order using the current order-update
mechanism instead of sending cancel on every RMF stop request.

## Docking Behavior

`RobotCommandHandle::dock()` currently completes immediately.

Docking is a placeholder and does not yet map `dock_name` to a VDA5050 custom
action. If docking or charging needs robot-specific behavior, this method should
be extended to publish an appropriate VDA5050 action and wait for action feedback.

## Battery And Charging

The adapter configures RMF task planner parameters using:

- `BatterySystem`
- `MechanicalSystem`
- `SimpleMotionPowerSink`
- `SimpleDevicePowerSink` for ambient power
- `SimpleDevicePowerSink` for tool power

Battery values are configured through `config/params.yaml`. The current defaults
represent a TurtleBot3 Burger.

`FleetAdapter::_add_robot()` scans the graph for the first waypoint marked with
`is_charger: true`. If found, that waypoint is assigned as the robot charger via
`RobotUpdateHandle::set_charger_waypoint()`.

## Configuration Files

### `config/params.yaml`

This file contains default ROS parameters for the adapter.

Key groups:

- fleet identity
- navigation graph path
- RMF websocket server URI
- MQTT broker URL
- VDA5050 identity
- map name
- vehicle traits
- battery and power model

### `maps/nav_graph.yaml`

This is the RMF navigation graph in building YAML format. The current map level
is `tb3_world`. Waypoints include:

- `initial_wp`
- `wp1_charging`
- `wp2_parking`
- `wp3`
- `wp4`
- `wp5`
- `wp6`

`wp1_charging` is marked as both charger and parking spot. Some lanes are defined
in both directions by adding one directed lane for each direction.

### `launch/fleet_adapter.launch.py`

The launch file starts the `vda5050_fleet_adapter` executable and loads:

- `config/params.yaml`
- launch overrides for `fleet_name`, `rmf_server_uri`, `mqtt_broker_url`
- installed `maps/nav_graph.yaml` as `nav_graph_path`

## Helper Scripts

### `scripts/dispatch_patrol.py`

Publishes an RMF `dispatch_task_request` to `/task_api_requests`. It can dispatch
a patrol task to one or more named waypoints:

```bash
python3 ~/ros2_ws/src/vda5050_fleet_adapter/scripts/dispatch_patrol.py wp2_parking
python3 ~/ros2_ws/src/vda5050_fleet_adapter/scripts/dispatch_patrol.py wp2_parking wp1_charging initial_wp --rounds 2
```

The script waits for an RMF dispatcher subscriber before publishing, then tries to
read the assigned `task_id` from `/task_api_responses`.

### `scripts/cancel_task.py`

Publishes an RMF `cancel_task_request` to `/task_api_requests`:

```bash
python3 ~/ros2_ws/src/vda5050_fleet_adapter/scripts/cancel_task.py patrol.dispatch-3
```

### `scripts/visualize_nav_graph.py`

Publishes `visualization_msgs/MarkerArray` on `/nav_graph_markers` so the graph
can be inspected in RViz.

Current CMake install state:

- `visualize_nav_graph.py` is installed as a ROS executable.
- `dispatch_patrol.py` and `cancel_task.py` are source-tree helper scripts and
  are not currently installed by `CMakeLists.txt`.

## Build And Run

Build the package from the workspace root:

```bash
cd ~/ros2_ws
colcon build --packages-select vda5050_fleet_adapter
source install/setup.bash
```

Start an MQTT broker first:

```bash
sudo systemctl start mosquitto
```

Launch the fleet adapter:

```bash
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py
```

Override selected values when needed:

```bash
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py \
  fleet_name:=tb3_fleet \
  mqtt_broker_url:=tcp://localhost:1883 \
  rmf_server_uri:=
```

Dispatch a patrol task:

```bash
python3 ~/ros2_ws/src/vda5050_fleet_adapter/scripts/dispatch_patrol.py wp6
```

## Integration Assumptions

For the full system to work, these values must agree across packages:

- MQTT broker URL
- VDA5050 `interface_name`
- VDA5050 `manufacturer`
- VDA5050 `serial_number`
- map name / map id
- waypoint names used in RMF dispatch requests

The expected robot-side behavior is:

- subscribe to VDA5050 `order`;
- execute each order node and edge;
- publish VDA5050 `state`;
- include `agvPosition` with the same map id used by RMF;
- update `lastNodeId` as nodes are reached;
- set `driving` to false and clear `nodeStates` when the order is complete.

## Current Limitations

- Only one robot is registered by this package instance.
- `dock()` is a placeholder and completes immediately.
- `instantActions` topic is prepared but not actively used by the current stop
  flow.
- The adapter uses MQTT QoS 0 for order publishing.
- Graph parsing currently uses the first level in the YAML file.
- If no charger waypoint is present, RMF battery charging cannot be assigned.
- `dispatch_patrol.py` and `cancel_task.py` are not installed by CMake yet.

## File Map

```text
vda5050_fleet_adapter/
  CMakeLists.txt
  package.xml
  config/
    params.yaml
  launch/
    fleet_adapter.launch.py
  maps/
    nav_graph.yaml
    tb3_world.building.yaml
  include/vda5050_fleet_adapter/
    fleet_adapter.hpp
    robot_command_handle.hpp
  src/
    main.cpp
    fleet_adapter.cpp
    robot_command_handle.cpp
  scripts/
    dispatch_patrol.py
    cancel_task.py
    visualize_nav_graph.py
    mock_robot_driver.py
  docs/
    architecture.md
```
