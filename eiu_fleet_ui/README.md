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
| Robot Control dialog | Pause/resume, speed-limit override, re-localize (click-on-map or typed pose), direct "go to waypoint" pinned to that one robot. A command shows as waiting (PAUSING…, APPLYING…) until the fleet adapter answers, cannot be sent twice meanwhile, and fails with a message if no answer comes within `commands.timeout_s` |
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
| `dashboard.py` | `Dashboard`: what the panels show. On the GUI thread, every `dashboard.refresh_period_s` it takes one snapshot of the backends, derives the views with `dashboard_model.py`, and hands QML keyed list models (robots, map markers, Needs Attention, tasks, VDA5050 traffic) plus values that notify only when they change. `UiConfig` exposes the operator thresholds and map limits. |
| `dashboard_model.py` | Pure (no Qt): merges RMF's view, VDA5050 telemetry and the config into robot rows, builds the Needs Attention items with stable keys, system health, fleet summary, task filters and counts, and the route polylines the map draws. |
| `list_model.py` | `KeyedListModel`: a `QAbstractListModel` whose rows are keyed; a new list inserts, removes, moves and updates rows in place, so views keep their delegates, scroll position, hover and running animations. Also registered for QML as `EiuFleet.KeyedListModel`. |
| `task_state.py` | Pure: the one place that sets a task's state. Each source (API answers and task events, `/dispatch_states`, a robot's task id in `/fleet_states`, the dashboard's own timeouts) has a rank; a better-informed source may change a state either way, an equal or weaker one only moves it forward. Task times are epoch milliseconds; an expected end is kept apart from the real one. |
| `ui_settings.py` | The dashboard's tunables from `config/ui_settings.yaml`, each with a default and a valid range, overridden by the `EIU_*` variables below. |
| `config.py` | Reads the fleet adapter's own config file(s) (one or more, see Multi-fleet) for broker/robot/task-category settings. |
| `map_provider.py` | Reads `map.yaml` (SLAM origin + resolution) and `nav_graph.yaml` (waypoints + lanes). Converts the grayscale PGM map to ARGB32 PNG for QML rendering. Exposes `wpJson` and `lanesJson`. |
| `ros_bridge.py` | Subscribes to `/fleet_states`, `/task_api_responses`, `/dispatch_states`, `/lane_states`, `/rmf_traffic/negotiation_statuses`. Publishes task dispatch/cancel and lane-closure requests, woken by a guard condition rather than a polling timer. Keeps the robots and tasks as plain records the dashboard snapshots; tracks each fleet's last `/fleet_states` so a fleet that goes silent is reported on its own. |
| `robot_registry.py` | Robot registration over ROS: reads each fleet's `/robot_registry` and `/robot_discovery`, sends add/remove/dry-run requests on `/robot_registration_requests` and routes the answers, with a timeout when no adapter replies. Announces registered robots to `FleetSettings`, `MqttClient`, `RosControl` and `RosBridge`. |
| `adapter_metrics.py` | Follows each fleet adapter's `/<adapter node>/metrics` report (JSON) and offers the snapshot and the attention items to QML. `EIU_METRICS_HISTORY` (samples kept, default 120) and `EIU_METRICS_SILENT_FACTOR` (reporting intervals before an adapter counts as silent, default 3) and `EIU_ADAPTER_GRACE_S` (seconds after start before a missing adapter is reported, default 10) tune it. |
| `metrics_model.py` | The pure part of it (no Qt, no ROS): per-interval rates from the adapter's running counts, the series for the charts, and the attention items. Every limit it compares against comes from the report itself. |
| `registry_model.py` | Pure: follows registered robots, works out pending ones (incl. a removed robot online again, via `removed_as`), suggests fleet/name/charger. |
| `ros_control.py` | Direct per-robot control: pause/resume (services), speed-limit override (ROS param, one parameter client per fleet adapter node), re-localize (`init_position` topic) — wired to each robot's own fleet adapter node. Tracks which commands still wait for an answer and fails them after `commands.timeout_s`. |
| `mqtt_client.py` | Connects to the MQTT broker; subscribes to each robot's `connection`, `state`, `order` and `instantActions` topics; keeps a bounded traffic log for the VDA5050 Traffic tab. `snapshot()` gives the dashboard connection, telemetry (re-exported only for robots with new state) and the log. Parsing lives in `vda5050/state.py`. |
| `vda5050/state.py` | Pure VDA5050 `state` message parsing — no Qt, no I/O. Mirrors `vda5050_fleet_adapter_full_control`'s own `ParsedState` field-for-field. |
| `vda5050/traffic.py` | Pure: per-action blocking type, trimmed order for the node-details view, traffic-log summaries. |
| `vda5050/graph.py` | Pure: distance of a robot's pose from the nav graph, and off-graph duration. |
| `graph_editor.py` | Editable working copy of `nav_graph.yaml` exposed to QML; *Save as* writes it via `file_io.write_atomic`. |
| `file_io.py` | Dashboard state location, atomic file writer (keeps mode/owner), debounced background writer. |
| `task_websocket.py` | Optional `TaskEventServer` — receives `task_state_update`/`task_log_update` events from the fleet adapter over a WebSocket, when `vda5050.ui_websocket_uri` is configured. |

## QML frontend files

| File | Role |
|:---:|---|
| `qml/main.qml` | Root window (opens maximized): lays out the panels below and the dialogs, and keeps the robot selection shared by the map, the robot list and the analytics. |
| `qml/pages/MapPage.qml` | Zoomable/pannable map: lanes, waypoints, robot markers that glide between updates, route and destination per robot, no-go zone drawing, nav graph editor overlay (`EDIT GRAPH`). The graph layer and the route layer are separate canvases. Waypoint labels are always shown, placed around their pin where they overlap least. |
| `qml/components/NavRail.qml`, `TopBar.qml`, `StatusChip.qml`, `KpiRow.qml` | Sidebar, title bar with the RMF/MQTT/adapters/alerts chips, and the KPI cards. |
| `qml/components/MapCard.qml` | The map above Fleet Analytics; the map keeps its natural height up to 60 % of the column, analytics scrolls when it needs more room. |
| `qml/components/NeedsAttentionPanel.qml`, `RobotListPanel.qml`, `TasksPanel.qml` | The right-hand column: issues, robots, tasks and the VDA5050 log, each on a keyed model from `dashboard`. |
| `qml/components/Theme.qml` | The palette and the colors given to robot, task and health states (QML singleton, declared in `qml/components/qmldir`). |
| `qml/components/Format.js` | Time texts shared by the panels ("12s ago", local task dates and clock times). |
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

Timings, limits and thresholds live in [`config/ui_settings.yaml`](config/ui_settings.yaml), which lists
every key with its default (`EIU_UI_CONFIG=/path/to/file.yaml` reads another file). Each key has a
valid range; a value of the wrong type or out of range, and a key the dashboard does not know, is
reported and the default is used. Where a variable is named, it overrides the file:

| Variable | Key | Default | Effect |
|:---:|:---:|:---:|---|
| `EIU_UI_REFRESH_PERIOD` | `dashboard.refresh_period_s` | 0.2 s | How often the panels show new data; robot markers glide in between |
| `EIU_REGISTRATION_TIMEOUT` | `registration.reply_timeout_s` | 10 s | How long the Register dialog waits for a fleet adapter's answer |
| `EIU_DISPATCH_TIMEOUT` | `rmf.dispatch_timeout_s` | 15 s | A dispatch without an RMF task id, or a cancel RMF does not answer, fails after this |
| `EIU_COMMAND_TIMEOUT` | `commands.timeout_s` | 10 s | Pause, resume, speed limit or re-localize without an answer fails after this |
| `EIU_RMF_OFFLINE_AFTER` | `rmf.offline_after_s` | 5 s | RMF (or one fleet, while others still report) is shown offline after this long without `/fleet_states` |
| `EIU_STATE_STALE_AFTER` | `vda5050.state_stale_after_s` | 5 s | A robot's telemetry is marked stale after this long without a `state` |
| `EIU_TASK_HISTORY` | `tasks.history` | 50 | Tasks kept in the table and in the cache |
| `EIU_TASK_CACHE_DELAY` | `tasks.cache_write_delay_s` | 1 s | Task changes are written to the cache at most this often, off the UI thread |
| `EIU_METRICS_HISTORY`, `EIU_METRICS_SILENT_FACTOR`, `EIU_ADAPTER_GRACE_S` | `adapter_metrics.*` | 120, 3, 10 s | System view history and when an adapter counts as silent or missing |

The file alone sets the battery thresholds (`operator.*`), the off-graph distance and hold time,
the traffic log size, the default level name, and the map's pick radii and zoom limits.

Other locations:

| Variable | Default | Effect |
|:---:|:---:|---|
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
| `cancelled` | Grey | Cancelled by user or system |
| `failed` | Red | Task failed (obstacle, timeout, etc.) |

The state comes from several RMF sources that arrive in any order; `task_state.py` ranks them
(task events and API answers above `/dispatch_states`, above a robot dropping its task id) so a
lagging message cannot move a task back, while a guess such as "the robot dropped the task, so
it completed" is corrected by RMF's own verdict. Times are shown in local time; an end time RMF
only expects is marked with `~`.

## Checking the UI without a display

| Tool | What it checks |
|:---:|---|
| `tools/responsive_check.py` | Renders the dashboard offscreen at ten window sizes (1120x700 up to 5120x1440), reports clipped text and overlapping waypoint labels, and saves a screenshot per size. |
| `tools/load_check.py` | Drives the dashboard with synthetic fleets (`--fleets`, `--robots`, `--fleet-hz`, `--state-hz`) through the same callbacks ROS and MQTT use, and reports Qt Quick items created per second, whether the robot list keeps its scroll position, CPU use and QML warnings (`--report file.json`). |

Both need only PySide6, no ROS or broker.

## Reviewer recommendations

| Recommendation | Status |
|:---:|---|
| 🔵 UI: surface the VDA5050 traffic | ✅ Done — [`VdaOrderPanel.qml`](qml/components/VdaOrderPanel.qml) shows the selected robot's live order (route tiles, order/update id, running action, per-action blocking on a clicked node), and [`VdaTrafficPanel.qml`](qml/components/VdaTrafficPanel.qml) is a filterable log of order/instantAction/state/connection messages with raw JSON on click. Multi-node orders, `orderUpdateId` increments, per-action blocking, and `cancelOrder` against a pause are now watchable, not just taken on trust |
