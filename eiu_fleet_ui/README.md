# EIU Fleet UI

EIU Fleet UI is a desktop dashboard for [Open-RMF](https://github.com/open-rmf/rmf), built with PySide6 and QML.
It dispatches and cancels tasks, and shows the data of the fleet adapters: robot status, the navigation map, VDA5050 messages and adapter metrics.
It supports the development and testing of the VDA5050 fleet adapter, for one or more fleets.

<p align="center">
  <img src="icons/eiu.png" alt="EIU Fleet UI logo" />
</p>

![Dashboard overview](../assets/img/dashboard.png?v=2)

System design, component diagrams, data flows and topics: [docs/architecture.md](docs/architecture.md).

---

## Key features

| Feature | What it does |
|:---:|---|
| Live map | Map, nav graph, blocked lanes, robots and their routes |
| System overview | Health, robots, traffic and tasks at a glance; Needs Attention lists every problem |
| Robot status | Connection, battery, task and telemetry of each robot |
| Task dispatch | Create and cancel patrol and delivery tasks |
| Robot control | Pause, resume, speed limit, set position, go to waypoint |
| No-go zones | Close the lanes inside a rectangle drawn on the map |
| Robot registration | Add robots found on the broker to a fleet |
| VDA5050 view | Active order of a robot and a log of VDA5050 messages |
| Adapter health | Metrics of every fleet adapter |
| Nav graph editor | Edit waypoints and lanes, save `nav_graph.yaml` |

The dashboard can follow several fleet adapters at once (see [Multiple fleets](#multiple-fleets)).

---

## Package layout

```
eiu_fleet_ui/    Python backend: ROS and MQTT clients, models for the panels
  vda5050/         parsing of VDA5050 state, traffic log, distance from the nav graph
qml/             QML frontend: main.qml, pages/ and components/
config/          ui_settings.yaml
maps/            occupancy map and nav graph
launch/          eiu_fleet_ui.launch.py
icons/, logo/    robot icons and logos
fonts/           IBM Plex fonts
test/            unit tests
tools/           load_check.py, a load test of the dashboard
docs/            architecture.md
```

## Prerequisites

| Dependency | Version | Notes |
|:---:|:---:|---|
| ROS 2 | Jazzy | Same container as the fleet adapter; uses `rclpy`, `rmf_fleet_msgs` and `rmf_task_msgs` |
| PySide6 | ≥ 6.4 | `pip install PySide6` or `apt install python3-pyside6` |
| paho-mqtt | ≥ 1.6 | `pip install paho-mqtt` |
| PyYAML | any | `pip install pyyaml` |
| Open-RMF | Jazzy | Same container as this UI; see `fleet_bringup` |
| MQTT broker | Mosquitto | `localhost:1883` |

> **ROS domain:** the UI, Open-RMF and the fleet adapter run in the same container and share its `ROS_DOMAIN_ID`. `EIU_ROS_DOMAIN_ID` selects another domain for the UI.

---

## Build & Run

```bash
# 1. Build
cd ~/ros2_ws
colcon build --packages-select eiu_fleet_ui
source install/setup.bash

# 2. Start Open-RMF, the fleet adapter and the MQTT broker
#    (see vda5050_fleet_adapter_full_control/README.md)

# 3. Run with the config of the fleet adapters in use
# Real robots: config_tb3.yaml and config_amr.yaml (default)
ros2 launch eiu_fleet_ui eiu_fleet_ui.launch.py

# Gazebo simulation: config_tb3_sim.yaml (broker on localhost)
ros2 launch eiu_fleet_ui eiu_fleet_ui.launch.py \
    fleet_adapters:=/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_tb3_sim.yaml=vda5050_fleet_adapter_tb3

# Virtual AGVs (vda-5050-lib)
ros2 launch eiu_fleet_ui eiu_fleet_ui.launch.py \
    fleet_adapters:=/ros2_ws/src/vda5050_fleet_adapter_full_control/test/third_party/config_virtual_agv.yaml=vda5050_fleet_adapter_virtual

# Optional: another ROS domain than the shell's ROS_DOMAIN_ID
ros2 launch eiu_fleet_ui eiu_fleet_ui.launch.py ros_domain_id:=10
```

### Multiple fleets

The default launch already follows both fleets, `tb3_fleet` and `amr_fleet`. To choose other fleets, list each adapter as `config_file=node_name`, separated by commas; the `node_name` must match the one the adapter was launched with:

```bash
ros2 launch eiu_fleet_ui eiu_fleet_ui.launch.py \
    fleet_adapters:=/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_tb3.yaml=vda5050_fleet_adapter_tb3,/ros2_ws/src/vda5050_fleet_adapter_full_control/config/config_amr.yaml=vda5050_fleet_adapter_amr
```

### ui_settings.yaml

The dashboard reads its timings, limits and thresholds from [`config/ui_settings.yaml`](config/ui_settings.yaml). To use another file, set `EIU_UI_CONFIG` to its path. If a value has the wrong type, is out of range or belongs to an unknown key, the dashboard reports it and uses the default. An environment variable, where the table names one, overrides the value in the file.

| Key | Variable | Default | Range | Effect |
|:---:|:---:|:---:|:---:|---|
| `dashboard.refresh_period_s` | `EIU_UI_REFRESH_PERIOD` | 0.2 s | 0.02–5 | Panel refresh period |
| `rmf.offline_after_s` | `EIU_RMF_OFFLINE_AFTER` | 5 s | 0.5–600 | Time without `/fleet_states` before RMF or a fleet is shown offline |
| `rmf.dispatch_timeout_s` | `EIU_DISPATCH_TIMEOUT` | 15 s | 1–600 | Time to wait for RMF to answer a dispatch or cancel |
| `rmf.default_level` | — | `L1` | | Level shown when a fleet reports none |
| `tasks.history` | `EIU_TASK_HISTORY` | 50 | 1–10000 | Tasks kept in the table and the task cache |
| `tasks.cache_write_delay_s` | `EIU_TASK_CACHE_DELAY` | 1 s | 0.01–60 | Minimum time between writes of the task cache |
| `commands.timeout_s` | `EIU_COMMAND_TIMEOUT` | 10 s | 1–600 | Time to wait for an answer to pause, resume, speed limit or re-localize |
| `registration.reply_timeout_s` | `EIU_REGISTRATION_TIMEOUT` | 10 s | 1–600 | Time to wait for an answer to a registration request |
| `vda5050.state_stale_after_s` | `EIU_STATE_STALE_AFTER` | 5 s | 0.5–600 | Time without a VDA5050 state before a robot is shown offline |
| `vda5050.stale_state_intervals` | `EIU_STALE_STATE_INTERVALS` | 2 | 1–100 | AGV state intervals without a state before the robot is shown offline, if longer. The interval is the factsheet's `defaultStateInterval`, or 30 s |
| `vda5050.traffic_log_size` | — | 200 | 10–100000 | Messages kept in the VDA5050 traffic log |
| `vda5050.off_graph_limit_m`, `off_graph_hold_s` | — | 1.2 m, 5 s | | Distance from every lane, and time, before a robot is reported off the graph |
| `adapter_metrics.history_samples` | `EIU_METRICS_HISTORY` | 120 | 2–100000 | Metrics reports kept per fleet adapter for the charts |
| `adapter_metrics.silent_factor` | `EIU_METRICS_SILENT_FACTOR` | 3 | 1–100 | Missed report periods before an adapter is shown as silent |
| `adapter_metrics.grace_s` | `EIU_ADAPTER_GRACE_S` | 10 s | 0–600 | Time after start before a missing adapter is reported |
| `operator.low_battery_percent`, `medium_battery_percent` | — | 20, 50 | 0–100 | Battery levels shown in red and amber |
| `map.*` | — | | | Click distances for waypoints and lanes, and zoom limits |

### Other environment variables

These variables have no key in `ui_settings.yaml`. They set where the dashboard reads the nav graph, where it keeps its files, and where the task-event WebSocket listens.

| Variable | Default | Effect |
|:---:|:---:|---|
| `EIU_NAV_GRAPH` | graph beside the adapter config | Nav graph shown on the map and used by the graph editor; also `nav_graph:=` of the launch file |
| `EIU_CONFIG_DIR` | `$XDG_CONFIG_HOME/eiu_fleet_ui`, else `~/.config/eiu_fleet_ui` | Folder for the task cache |
| `EIU_WS_BIND` | host of `ui_websocket_uri` | Address of the task-event WebSocket; `any` listens on every interface |

---

## Map & nav-graph configuration

The map page draws the occupancy map with the nav graph on top. The files are in `eiu_fleet_ui/maps/`; the nav graph can also come from the fleet adapter (see `EIU_NAV_GRAPH` above).

| File | Format | Purpose |
|:---:|:---:|---|
| `map.yaml` | ROS map_server YAML | SLAM map metadata (origin, resolution, image path) |
| `map.pgm` / `map.png` | PGM / PNG | SLAM occupancy grid image |
| `nav_graph.yaml` | Open-RMF traffic editor | Waypoints (vertices) and lanes (edges) |
| `tb3_world.building.yaml` | Open-RMF building | Building definition used by rmf_traffic_schedule |

To use a different map, replace the files in `maps/` and rebuild.

---

## Topics

The MQTT and ROS 2 topics of the dashboard are listed in [docs/architecture.md](docs/architecture.md#interfaces).

---

## Reviewer recommendations

| Recommendation | Status |
|:---:|---|
| 🔵 UI: surface the VDA5050 traffic | ✅ Done — [`VdaOrderPanel.qml`](qml/components/VdaOrderPanel.qml) shows the selected robot's live order (route tiles, order/update id, running action, per-action blocking on a clicked node), and [`VdaTrafficPanel.qml`](qml/components/VdaTrafficPanel.qml) is a filterable log of order/instantAction/state/connection messages with raw JSON on click. Multi-node orders, `orderUpdateId` increments, per-action blocking and `cancelOrder` during a pause can be observed |
