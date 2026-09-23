# Architecture

## System view

The simulation runs in the `tb3-simulation` container on `ROS_DOMAIN_ID=1`; the fleet adapter, dispatcher and dashboard run in a separate container on `ROS_DOMAIN_ID=10`. The two ROS graphs never see each other — MQTT is the only link, the same boundary that exists with real AGVs.

```
              ROS_DOMAIN_ID=10  (fleet container)
    ┌──────────────────────────────────────────────┐
    │  rmf_traffic_schedule    rmf_task_dispatcher │
    │              └──────┬──────┘                 │
    │                fleet_adapter      eiu_fleet_ui
    └─────────────────────┬────────────────────────┘
                          │  VDA5050 over MQTT
                          │  order / state / instantActions
                 ┌────────┴────────┐
                 │   MQTT broker   │  tcp://localhost:1883
                 └────────┬────────┘
    ┌─────────────────────┴────────────────────────┐
    │        ROS_DOMAIN_ID=1  (sim container)      │
    │                                              │
    │  vda5050_client_adapter_node  ── per robot   │
    │     │ order            ▲ state, node_reached │
    │     ▼                  │                     │
    │  tb3_vda5050_bridge_node      ── per robot   │
    │     │ navigate_to_pose (action)              │
    │     ▼                                        │
    │  Nav2 stack                   ── per robot   │
    │     │ cmd_vel          ▲ odom, scan, tf      │
    │     ▼                  │                     │
    │  ros_gz_bridge      clock_bridge ──► /clock  │
    │     │                  ▲          (all Nav2) │
    │     ▼                  │                     │
    │  Gazebo (gz sim) ──────┘                     │
    └──────────────────────────────────────────────┘
```

## Nodes

`robot_count` selects how many of `tb3_1..3` come up. Per-robot nodes are pushed into the robot's namespace.

From `tb3_simulation_nav2.launch.py`, once per simulation:

- `clock_bridge` — bridges Gazebo's clock to ROS `/clock`; every `use_sim_time` node depends on it
- Gazebo server, optional GUI, one RViz bound to `tb3_1`

Per robot:

- `ros_gz_sim create` — spawns the Burger SDF with sensor topics rewritten to `/tb3_N/...`
- `parameter_bridge` — `joint_states`, `odom`, `tf`, `cmd_vel`, `imu`, `scan`
- `robot_state_publisher`, mock `battery_state` at 1 Hz
- Nav2 bringup — `map_server`, `amcl`, `planner_server`, `controller_server`, `smoother_server`, `behavior_server`, `bt_navigator`, `waypoint_follower`, `velocity_smoother`, `collision_monitor`, `docking_server`, `route_server`, two `lifecycle_manager`

From `vda5050_bridge_fleet.launch.py`, per robot:

- `vda5050_client_adapter_node` — VDA5050 over MQTT; identity `ROBOTIS` / serial `0001..0003`
- `tb3_vda5050_bridge_node` — order nodes → `navigate_to_pose`; reports `node_reached`, `edge_entered`, `edge_completed`
- `mock_load_publisher` — stands in for a load sensor, feeding VDA5050 `state.loads`

## Velocity path

```
controller_server ─cmd_vel_nav─► velocity_smoother ─cmd_vel_smoothed─►
    collision_monitor ─cmd_vel─► parameter_bridge ─► Gazebo
```

All three ROS stages publish `TwistStamped`, matching the bridge's `TwistStamped]gz.msgs.Twist` mapping.

## Startup order

Staggered so Gazebo, the GUI and three Nav2 stacks do not contend during bringup. `SPAWN_DELAY_OFFSET` (5 s) gives the Gazebo GUI time to attach — entities spawned before it connects are simulated but never rendered.

| t (s) | Action |
|---|---|
| 0 | `clock_bridge`, Gazebo, per-robot bridges, `robot_state_publisher` |
| 5 + 2·i | Spawn robot *i* |
| 15 + 2·i | Nav2 bringup for robot *i* |
| 19 | RViz |
| 31 + 2·i | Initial pose for robot *i* |

## Cross-component contracts

- One `nav2_params.yaml` serves every robot; `ReplaceString` rewrites `topic: /scan` to `topic: /tb3_N/scan` for both costmaps at launch.
- Velocity limits are declared three times — `nav2_params.yaml`, the fleet adapter's `rmf_fleet.limits`, and the VDA5050 factsheet. RMF schedules traffic from its own copy, so the three must agree.
