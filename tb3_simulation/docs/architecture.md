# Architecture

## System overview

The system uses two isolated ROS 2 domains. The ground-station container runs Open-RMF core, the fleet adapter, and the operator UI on `ROS_DOMAIN_ID=10`. The simulation container runs Gazebo, Nav2, the VDA5050 client adapters, and the TurtleBot3 bridges on `ROS_DOMAIN_ID=1`. MQTT transfers VDA5050 messages between the two domains.

<div align="center">
<pre style="display: inline-block; text-align: left;">
┌──────────────────────────────────────────────────────┐
│ Ground station · ROS_DOMAIN_ID=10                    │
│ Open-RMF schedule · task dispatcher · fleet adapter  │
│ eiu_fleet_ui                                         │
└──────────────────────────┬───────────────────────────┘
                           │ VDA5050 over MQTT
                           ▼
                 ┌─────────────────────┐
                 │ MQTT broker         │
                 │ localhost:1883      │
                 └──────────┬──────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────┐
│ Simulation · ROS_DOMAIN_ID=1                         │
│                                                      │
│ VDA5050 client adapter  (per robot)                  │
│            ↕ NavigateToNode and telemetry            │
│ TurtleBot3 bridge       (per robot)                  │
│            ↕ NavigateToPose and robot state          │
│ Nav2 stack              (per robot)                  │
│            ↕ velocity commands and sensor data       │
│ ROS-Gazebo bridge  ↔  Gazebo                         │
│ Gazebo  →  clock bridge  →  ROS /clock               │
└──────────────────────────────────────────────────────┘
</pre>
</div>

## Runtime components

The runtime architecture has four groups: ground-station services, MQTT transport, simulation services, and per-robot control stacks.

`robot_count` selects robots from `config/robot_poses.yaml`. Each robot uses an independent ROS namespace and Nav2 stack.

**Ground-station components**

- `rmf_traffic_schedule` manages the shared traffic schedule.
- `rmf_task_dispatcher` assigns RMF tasks.
- The VDA5050 fleet adapter converts RMF commands to VDA5050 messages and processes robot state.
- `eiu_fleet_ui` provides fleet status and task controls.
- `fleet_bringup` starts the ground-station components and optional mock workcells.

**MQTT transport**

- The MQTT broker routes VDA5050 orders, state, and instant actions between the two ROS domains.

**Simulation-wide components**

- Gazebo simulates the world and robot entities.
- `clock_bridge` publishes simulation time on ROS `/clock`.
- The Gazebo GUI provides optional world visualization.
- RViz provides optional Nav2 visualization for `tb3_1`.

**Per-robot components**

- `ros_gz_sim create` creates the TurtleBot3 Burger entity.
- `parameter_bridge` transfers `joint_states`, `odom`, `tf`, `cmd_vel`, `imu`, and `scan` between Gazebo and ROS 2.
- `robot_state_publisher` publishes robot transforms.
- Nav2 provides localization, planning, control, behavior execution, velocity smoothing, collision monitoring, and docking.
- The mock battery publisher publishes `battery_state` at 1 Hz.

Nav2 uses separate processes by default. `use_composition:=True` runs each robot stack in one `nav2_container` process.

VDA5050 integration is available for `tb3_1` to `tb3_3`:

- `vda5050_client_adapter_node` exchanges VDA5050 messages with MQTT.
- `tb3_vda5050_bridge_node` converts `NavigateToNode` steps to Nav2 `NavigateToPose` goals and publishes telemetry.
- `mock_load_publisher` publishes simulated load data in VDA5050 state messages.

Each configured robot uses manufacturer `ROBOTIS` and a unique serial number from `0001` to `0003`.

## Velocity path

```text
controller_server ─cmd_vel_nav─► velocity_smoother ─cmd_vel_smoothed─►
collision_monitor ─cmd_vel─► parameter_bridge ─► Gazebo
```

All velocity stages use `TwistStamped`. The Gazebo bridge maps the final command to `gz.msgs.Twist`.

## Startup order

Startup timers separate robot creation, Nav2 startup, RViz, and initial pose publication. Robot index `i` starts at 1.

| Time (s) | Action |
|---|---|
| 0 | Start Gazebo, `clock_bridge`, per-robot parameter bridges, robot state publishers, and battery publishers |
| 5 + 2i | Create robot *i* |
| 15 + 2i | Start Nav2 for robot *i* |
| 19 | Start RViz when enabled |
| 31 + 2i | Publish the initial pose for robot *i* |


## Cross-component contracts

- One `nav2_params.yaml` configures every robot. Launch-time replacement changes `topic: /scan` to `topic: /tb3_N/scan` for each namespace.
- Velocity limits must match in `nav2_params.yaml`, `config_tb3_sim.yaml`, and the VDA5050 factsheet.
- Each VDA5050 client requires a unique `mqtt.client_id` and a serial number that matches `config_tb3_sim.yaml`.
- Bridge topic parameters use absolute `/tb3_N/...` names. ROS namespace pushes do not modify absolute parameter values.
- `map_id` must match the Open-RMF map name `tb3_world`.
- Both containers must use host networking when the MQTT broker URL is `localhost:1883`.
