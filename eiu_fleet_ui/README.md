# EIU Fleet UI

A real-time fleet management dashboard for [Open-RMF](https://github.com/open-rmf/rmf) built with **PySide6 + QML**. Monitors robot status, visualizes the navigation map, and dispatches / cancels tasks across one or more robot fleets — all from a single desktop window.

<p align="center">
  <img src="icons/eiu.png" alt="EIU Fleet UI logo" />
</p>

![Dashboard overview](../assets/img/dashboard.png?v=2)

> See [docs/architecture.md](docs/architecture.md) for system design, component diagrams, and data-flow sequence diagrams.

---

## Features

| Area | What it does |
|:---:|---|
| Live navigation map | Occupancy grid + nav-graph overlay, lane direction arrows, blocked-lane highlighting from live RMF traffic state, multi-robot markers (per-robot-type icon) with heading/pulse, planned-path overlay, click-to-pick a pose or waypoint |
| Fleet Command dashboard | KPI cards (system health, fleet + VDA5050-connected count, traffic status, tasks), RMF/MQTT online indicators, a Needs Attention panel listing every active issue |
| Active Robots panel | Search/filter, battery, round progress, live telemetry badges (not-localized, no-recent-data, safety/eStop/fatal-error) |
| Robot Control dialog | Pause/resume, speed-limit override, re-localize (click-on-map or typed pose), direct "go to waypoint" pinned to that one robot |
| Recent Tasks table | Search/filter, underway-first sort, cancel button. A cancel shows CANCELLING until RMF confirms it, then CANCELLED; a refused or unanswered cancel shows CANCEL FAILED and can be retried. Every command carries a request id, so a dialog only reacts to its own request; a dispatcher that never responds fails the task after a timeout |
| New Task dialog | Fleet-wide patrol or delivery dispatch (destinations, loop count; delivery handlers are the dispenser and ingestor RMF reports, payload is typed in and remembered) — RMF bids it to whichever robot it picks, or send it straight to one robot; draggable |
| Fleet Analytics | Battery/task-distribution gauges, current task progress, live distance-since-last-node, eStop/safety status per robot, delivery pickup/dropoff wait countdown, AGV-reported load while a delivery is underway |
| System view | The **System** item in the sidebar opens a panel for every fleet adapter: robots online, oldest state age (against the adapter's own offline limit), messages per second, dropped messages, MQTT link, update-loop time, and charts of the last reports with a crosshair readout. Problems an adapter reports about itself (lost broker, dropped or unsendable messages, an update loop that fell behind, reports that stopped) also appear under Needs Attention. A top-bar chip (**ADAPTERS 1/2**) shows how many adapters are found, at once and before any report, by whether something publishes their metrics topic; an adapter nobody publishes for is listed as *not found* |
| Broker check | When the fleets' configs name different MQTT brokers, Needs Attention says so: the dashboard reads robot state from the first fleet's broker only (`EIU_MQTT_HOST` picks one for all) |
| Traffic awareness | Blocked lanes and active negotiation/conflict counts, read from real RMF topics, not inferred |
| No-go zones | Drag a rectangle on the map; every lane it crosses is closed via RMF's own lane-closure mechanism. Multiple zones at once, click to select, delete to reopen |
| Robot registration | A robot on the broker that belongs to no fleet raises a *New robot detected* entry under Needs Attention; the Register dialog prefills fleet/name/charger and shows the adapter's live checks. Registered robots join the dashboard at once and persist. *Remove from fleet* decommissions one; if it's still online it's offered again with its old name/charger prefilled to restore it |
| Nav graph editor | *Edit graph* on the map: add/move waypoints (optionally chargers), add lanes, *Save as* writes an `nav_graph.yaml`. Nothing is written until saved |
| VDA5050 order & traffic | Fleet Analytics shows the selected robot's live order (route tiles, order/update id, running action, per-action blocking on a clicked node); a Traffic tab logs order/instantAction/state/connection messages, filterable, with raw JSON on click |
| Multi-fleet | Point the UI at several fleet adapter processes at once (e.g. one robot type per fleet) — see `EIU_FLEET_ADAPTERS` under Build & Run |
| Resilience | VDA5050 telemetry staleness detection, malformed-MQTT-payload hardening, dispatch/cancel confirmation with timeout |

---

## Python backend modules

| Module | Role |
|:---:|---|
| `main.py` | Entry point — creates the Qt app, QML engine, wires context properties, starts backends. |
| `config.py` | Reads the fleet adapter's own config file(s) (one or more, see Multi-fleet) for broker/robot/task-category settings. |
| `map_provider.py` | Reads `map.yaml` (SLAM origin + resolution) and `nav_graph.yaml` (waypoints + lanes). Converts the grayscale PGM map to ARGB32 PNG for QML rendering. Exposes `wpJson` and `lanesJson`. |
| `ros_bridge.py` | Subscribes to `/fleet_states`, `/task_api_responses`, `/lane_states`, `/rmf_traffic/negotiation_statuses`. Publishes task dispatch/cancel and lane-closure requests. Tracks per-fleet state so multi-fleet setups address the right fleet. |
| `robot_registry.py` | Robot registration over ROS: reads each fleet's `/robot_registry` and `/robot_discovery`, sends add/remove/dry-run requests on `/robot_registration_requests` and routes the answers, with a timeout when no adapter replies. Announces registered robots to `FleetSettings`, `MqttClient`, `RosControl` and `RosBridge`. |
| `adapter_metrics.py` | Follows each fleet adapter's `/<adapter node>/metrics` report (JSON) and offers the snapshot and the attention items to QML. `EIU_METRICS_HISTORY` (samples kept, default 120) and `EIU_METRICS_SILENT_FACTOR` (reporting intervals before an adapter counts as silent, default 3) and `EIU_ADAPTER_GRACE_S` (seconds after start before a missing adapter is reported, default 10) tune it. |
| `metrics_model.py` | The pure part of it (no Qt, no ROS): per-interval rates from the adapter's running counts, the series for the charts, and the attention items. Every limit it compares against comes from the report itself. |
| `registry_model.py` | Pure: follows registered robots, works out pending ones (incl. a removed robot online again, via `removed_as`), suggests fleet/name/charger. |
| `ros_control.py` | Direct per-robot control: pause/resume (services), speed-limit override (ROS param), re-localize (`init_position` topic) — wired to each robot's own fleet adapter node. |
| `mqtt_client.py` | Connects to the MQTT broker; subscribes to each robot's `connection` and `state` topics; keeps a bounded traffic log for the VDA5050 Traffic tab. Parsing lives in `vda5050/state.py`. |
| `vda5050/state.py` | Pure VDA5050 `state` message parsing — no Qt, no I/O. Mirrors `vda5050_fleet_adapter_full_control`'s own `ParsedState` field-for-field. |
| `vda5050/traffic.py` | Pure: per-action blocking type, trimmed order for the node-details view, traffic-log summaries. |
| `vda5050/graph.py` | Pure: distance of a robot's pose from the nav graph, and off-graph duration. |
| `graph_editor.py` | Editable working copy of `nav_graph.yaml` exposed to QML; *Save as* writes it via `file_io.write_atomic`. |
| `file_io.py` | Dashboard state location, atomic file writer (keeps mode/owner), debounced background writer. |
| `task_websocket.py` | Optional `TaskEventServer` — receives `task_state_update`/`task_log_update` events from the fleet adapter over a WebSocket, when `vda5050.ui_websocket_uri` is configured. |
| `colors.py` | Single source of truth for the dark-theme color palette exposed as QML context property `C`. |

## QML frontend files

| File | Role |
|:---:|---|
| `qml/main.qml` | Root window (opens maximized). KPI row, Needs Attention, Active Robots table, Tasks table, map panel. |
| `qml/pages/MapPage.qml` | Zoomable/pannable map: lanes, waypoints, robot markers, planned path, no-go zone drawing, nav graph editor overlay (`EDIT GRAPH`). |
| `qml/pages/StatusPage.qml` | Placeholder page (not yet implemented). |
| `qml/components/NewTaskDialog.qml` | Dispatch a fleet-wide patrol or delivery task; *Dispatch* disabled with a "Still needed: …" hint until the form is complete. |
| `qml/components/RobotControlDialog.qml` | Side drawer for direct per-robot control (pause/resume, speed limit, re-localize, go-to-waypoint, remove a runtime-added robot from its fleet). |
| `qml/components/RegisterRobotDialog.qml` | Register a robot found on the broker: fleet, name, charger, optional map frame, and the fleet adapter's live verdict; prefills a removed robot's own name/charger. |
| `qml/components/GraphPromptDialog.qml` | Modal the graph editor reuses: new-waypoint name, lane direction, save-as filename. |
| `qml/components/VdaOrderPanel.qml` | Selected robot's live VDA5050 order in Fleet Analytics: route tiles, order/update id, running action, clicked-node detail. |
| `qml/components/VdaTrafficPanel.qml` | Filterable log of VDA5050 order/instantAction/state/connection messages, raw JSON on click. |
| `qml/components/FreshnessTag.qml` | The "last update Ns ago / stale" indicator used wherever data freshness matters. |
| `qml/components/SystemMetricsDialog.qml` | The System view: one card per fleet adapter with its figures and charts. It covers the whole dashboard (everything right of the navigation rail), scales its text, spacing and charts with the window's width (1x up to 1.7x) and with a zoom of 60% to 180% (buttons in its header, Ctrl + / Ctrl - / Ctrl 0, or Ctrl + mouse wheel; remembered between runs); Esc or Close returns to the dashboard. The figures wrap into 6, 3 or 2 columns and the charts into 4, 2 or 1 as the room allows. |
| `qml/components/MetricChart.qml` | A single-measure line chart (2 px line, hairline grid, latest value, optional limit line, crosshair readout). |
| `qml/components/Toast.qml` | Short message with an optional action, used for the new-robot notice and registration results. |
| `qml/components/FleetAnalytics.qml` | Battery/task gauges, per-robot delivery/task progress, VDA5050 order panel and task-distribution chart side by side. |
| `qml/components/RobotCard.qml` | Compact robot summary card. |
| `qml/components/MetricCard.qml` | Generic KPI card used by the dashboard's metric row. |

---

## Prerequisites

| Dependency | Version | Notes |
|:---:|:---:|---|
| ROS 2 | Jazzy | Runs in the same Docker container class as the fleet adapter (rclpy, rmf_fleet_msgs, rmf_task_msgs) |
| PySide6 | ≥ 6.4 | `pip install PySide6` or `apt install python3-pyside6` |
| paho-mqtt | ≥ 1.6 | `pip install paho-mqtt` |
| PyYAML | any | `pip install pyyaml` |
| Open-RMF | Jazzy | Runs in the same Docker container/process group as this UI — see `fleet_bringup` |
| MQTT broker | Mosquitto | `localhost:1883` |

> **ROS domain:** normally nothing to configure — `eiu_fleet_ui`, Open-RMF core,
> and the fleet adapter all run in the same container via `fleet_bringup`, so
> they share whatever `ROS_DOMAIN_ID` that container already has (see
> `vda5050_fleet_adapter_full_control/docker/run.sh`). `EIU_ROS_DOMAIN_ID`
> only matters if you run this UI against an Open-RMF instance in a
> *different* process/container than usual.

---

## Build & Run

```bash
# 1. Build
cd ~/ros2_ws
colcon build --packages-select eiu_fleet_ui
source install/setup.bash

# 2. Make sure Open-RMF (Jazzy Docker) and MQTT broker are running
#    See: vda5050_fleet_adapter_full_control/README.md

# 3. Run
ros2 run eiu_fleet_ui eiu_fleet_ui

# Optional: only if Open-RMF runs on a different domain than this shell's default
EIU_ROS_DOMAIN_ID=10 ros2 run eiu_fleet_ui eiu_fleet_ui
```

### Multiple fleets (heterogeneous robot types)

Each robot type runs its own `vda5050_fleet_adapter_full_control` process
(different config file, different ROS node name — see that package's
README). By default the UI only looks for a single fleet adapter node named
`vda5050_fleet_adapter_full_control`. To have it control robots across
several fleet adapters at once, set `EIU_FLEET_ADAPTERS` to a comma-separated
list of `config_file=node_name`:

```bash
EIU_FLEET_ADAPTERS="/path/to/config_tb3.yaml=vda5050_fleet_adapter_tb3,/path/to/config_amr.yaml=vda5050_fleet_adapter_amr" \
  ros2 run eiu_fleet_ui eiu_fleet_ui
```

The UI merges the robot lists, task categories, and nav graph from every
fleet listed; broker connection is assumed shared (same MQTT broker) and
taken from the first fleet. Each robot's pause/resume/speed/init_position
controls, and its no-go-zone lane closures, are addressed to its own real
fleet name/node — so this only works if every fleet adapter was actually
launched with the matching `node_name` (see `fleet_adapter.launch.py`'s
`node_name` arg).

Single-fleet setups don't need this — the UI auto-discovers one config file
and defaults its node name to `vda5050_fleet_adapter_full_control`. Override
either half individually with `EIU_FLEET_CONFIG=/path/to/config.yaml` and/or
`EIU_FLEET_ADAPTER_NODE=my_node_name`.

Timings, limits and locations can be changed from the environment; an invalid
value is reported and the default is used.

| Variable | Default | Effect |
|:---:|:---:|---|
| `EIU_REGISTRATION_TIMEOUT` | 10 s | How long the Register dialog waits for a fleet adapter's answer |
| `EIU_DISPATCH_TIMEOUT` | 15 s | A dispatch without an RMF task id, or a cancel RMF does not answer, fails after this |
| `EIU_RMF_OFFLINE_AFTER` | 5 s | RMF is shown offline after this long without `/fleet_states` |
| `EIU_STATE_STALE_AFTER` | 5 s | A robot's telemetry is marked stale after this long without a `state` |
| `EIU_TASK_HISTORY` | 50 | Tasks kept in the table and in the cache |
| `EIU_TASK_CACHE_DELAY` | 1 s | Task changes are written to the cache at most this often, off the UI thread |
| `EIU_NAV_GRAPH` | graph beside the adapter config | Nav graph file the map shows and the editor starts from and saves next to (`nav_graph:=` on `eiu_fleet_ui.launch.py`); point it at the file the adapters use |
| `EIU_CONFIG_DIR` | `$XDG_CONFIG_HOME/eiu_fleet_ui` | Where the dashboard keeps its state (task cache) |
| `EIU_WS_BIND` | host of `ui_websocket_uri` | Address the task-event WebSocket listens on (`any` for every interface) |

---

## Map & nav-graph configuration

All map files live in `eiu_fleet_ui/maps/`:

| File | Format | Purpose |
|:---:|:---:|---|
| `map.yaml` | ROS map_server YAML | SLAM map metadata (origin, resolution, image path) |
| `map.pgm` / `map.png` | PGM / PNG | SLAM occupancy grid image |
| `nav_graph.yaml` | Open-RMF traffic editor | Waypoints (vertices) and lanes (edges) |
| `tb3_world.building.yaml` | Open-RMF building | Building definition used by rmf_traffic_schedule |

To use a different map, replace the files in `maps/` and rebuild.

---

## MQTT topics (VDA5050)

Topic prefix per robot is `<interface_name>/v2/<manufacturer>/<serial>/`, read
from the fleet adapter's config file(s) and from the fleets' `/robot_registry`
for robots added at runtime — never hardcoded (see `config.py`,
`robot_registry.py`).

| Topic (leaf) | Direction | Content |
|:---:|:---:|---|
| `state` | Robot → UI | Full `ParsedState`-equivalent: `driving`, `paused`, `orderId`/`orderUpdateId`, `batteryState`, `velocity`, `safetyState`, `errors[]`, `operatingMode`, `nodeStates`/`edgeStates`/`actionStates`, `loads`, `maps`. Parsed by `vda5050/state.py`. |
| `connection` | Robot → UI | `connectionState: ONLINE/OFFLINE/CONNECTIONBROKEN` |

Not yet subscribed: `visualization` (10 Hz pose refinement — `/fleet_states`
already covers the map's needs) and `factsheet` (robot capabilities/limits).

---

## ROS 2 topics

| Topic | Type | Direction | Purpose |
|:---:|:---:|:---:|---|
| `/fleet_states` | `rmf_fleet_msgs/FleetState` | RMF → UI | Robot position, battery, mode, path |
| `/task_api_requests` | `rmf_task_msgs/ApiRequest` | UI → RMF | Dispatch and cancel tasks |
| `/task_api_responses` | `rmf_task_msgs/ApiResponse` | RMF → UI | Task state updates, rmf_id mapping |
| `/lane_states` | `rmf_fleet_msgs/LaneStates` | RMF → UI | Which lanes are currently closed, per fleet |
| `/lane_closure_requests` | `rmf_fleet_msgs/LaneRequest` | UI → RMF | Close/reopen lanes for a no-go zone |
| `/robot_registry` | `std_msgs/String` (JSON, latched) | fleet adapter → UI | Per fleet: robots, chargers and who uses them, type, limits, adapter node |
| `/robot_discovery` | `std_msgs/String` (JSON, latched) | fleet adapter → UI | Robots online on the broker that no fleet has registered |
| `/robot_registration_requests` | `std_msgs/String` (JSON) | UI → fleet adapter | Add (or dry-run) and remove requests |
| `/robot_registration_results` | `std_msgs/String` (JSON) | fleet adapter → UI | Verdict with errors and warnings for each request |
| `/<adapter node>/metrics` | `std_msgs/String` (JSON) | fleet adapter → UI | Health of the adapter's message path, every `vda5050.metrics_period_s` (see the adapter's `docs/architecture.md`, "Metrics") |

See [docs/architecture.md](docs/architecture.md#process-boundaries-at-a-glance)
for the full protocol boundary table, including per-robot control topics.

---

## Task states

| State | Color | Meaning |
|:---:|:---:|---|
| `queued` | Yellow | Dispatched, waiting for a robot to pick up |
| `underway` | Blue | Robot actively executing the task |
| `completed` | Green | Task finished successfully |
| `cancelled` | Red | Cancelled by user or system |
| `failed` | Red | Task failed (obstacle, timeout, etc.) |

## Reviewer recommendations

| Recommendation | Status |
|:---:|---|
| 🔵 UI: surface the VDA5050 traffic | ✅ Done — [`VdaOrderPanel.qml`](qml/components/VdaOrderPanel.qml) shows the selected robot's live order (route tiles, order/update id, running action, per-action blocking on a clicked node), and [`VdaTrafficPanel.qml`](qml/components/VdaTrafficPanel.qml) is a filterable log of order/instantAction/state/connection messages with raw JSON on click. Multi-node orders, `orderUpdateId` increments, per-action blocking, and `cancelOrder` against a pause are now watchable, not just taken on trust |
