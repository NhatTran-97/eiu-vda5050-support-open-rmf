# EIU Fleet UI — Architecture

A PySide6 + QML desktop dashboard for an Open-RMF fleet running the VDA5050
protocol. It watches RMF's own topics for fleet-level truth (position, task
state, traffic) and the robot's MQTT `state` topic for everything RMF doesn't
carry (speed, safety, battery detail, order progress) — then dispatches,
cancels, and directly controls robots through the same channels.

## Features

| Area | What it does |
|:---:|---|
| Live navigation map | Occupancy grid + nav-graph overlay, lane direction arrows, blocked-lane highlighting from live RMF traffic state, multi-robot markers with heading/pulse that glide between updates, each robot's route and task destination, click-to-pick a pose or waypoint |
| Fleet Command dashboard | KPI cards (system health: Healthy/Degraded/Critical/Offline, fleet + VDA5050-connected count, traffic status, tasks), RMF/MQTT online indicators, a Needs Attention panel listing every active issue |
| Active Robots panel | Search/filter, battery, round progress, live telemetry badges (not-localized, no-recent-data, safety/eStop/fatal-error) |
| Robot Control dialog | Pause/resume, speed-limit override, re-localize (click-on-map or typed pose), direct "go to waypoint" pinned to that one robot; a command shows as waiting until the adapter answers and fails after `commands.timeout_s` |
| Recent Tasks table | Search/filter, underway-first sort, cancel button, dispatch confirmed synchronously with error feedback and a server-side timeout for a dispatcher that never responds |
| New Task dialog | Fleet-wide patrol or delivery dispatch (destinations, handlers, loop count) — RMF bids it to whichever robot it picks; draggable |
| Fleet Analytics | Battery/task-distribution gauges, current task progress, live distance-since-last-node, eStop/safety status per robot, delivery pickup/dropoff wait countdown, AGV-reported load while a delivery is underway |
| System view | Per fleet adapter: robots online, oldest state age against the adapter's own offline limit, messages per second, drops, MQTT link, update-loop time, and charts of the last reports; the problems an adapter reports about itself also join Needs Attention |
| Traffic awareness | Blocked lanes and active negotiation/conflict counts, read from real RMF topics, not inferred |
| No-go zones | Drag a rectangle on the map; every lane it crosses is closed via RMF's own lane-closure mechanism. Multiple zones at once, click to select, delete to reopen |
| Robot registration | Unregistered broker robots raise a Needs Attention entry; Register dialog shows the adapter's live checks; a removed-but-still-online robot is offered again to be restored |
| Nav graph editor | Add/move waypoints and lanes on the map, *Save as* writes an `nav_graph.yaml`; nothing written until saved |
| VDA5050 order & traffic | Selected robot's live order as route tiles + per-action blocking; filterable log of order/instantAction/state/connection messages with raw JSON |
| Resilience | VDA5050 telemetry staleness detection, malformed-MQTT-payload hardening, dispatch/cancel confirmation with timeout |

## System architecture

Two machines, three ROS graphs meeting at one MQTT broker:

```mermaid
flowchart LR
    subgraph GS ["🖥️ Ground station — rmf_jazzy_vda_dev container, ROS_DOMAIN_ID=42"]
        direction TB
        subgraph UI ["eiu_fleet_ui"]
            direction LR
            QML["QML frontend"]
            PY["Python backend"]
            QML <--> PY
        end
        RMF["Open-RMF core\ntraffic_schedule · task_dispatcher\nmutex_group_supervisor"]
        FA["vda5050_fleet_adapter_full_control"]
    end

    MQ[("Mosquitto\nVDA5050 order/state JSON")]

    subgraph RB ["🤖 Robot — SOM-RK3399v2"]
        direction TB
        CA["vda5050_client_adapter"]
        BR["tb3_vda5050_bridge"]
        N2["Nav2 + AMCL"]
        CA <--> BR
        BR <--> N2
    end

    PY <-->|"ROS 2: topics, services, params"| RMF
    PY <-->|"ROS 2: pause/resume, speed_limit,\ninit_position"| FA
    RMF <-->|FullControl API| FA
    FA <--> MQ
    MQ <--> CA
    FA -.->|"websocket task events\n(optional, if configured)"| PY
    PY -->|MQTT subscribe: state, connection| MQ
```

RMF's own frame (`/fleet_states`) and the robot's VDA5050 frame (MQTT
`state`) are two independent sources of truth about the same robot; the UI
never lets one silently overwrite the other — it merges them per field (see
`display_robots` in `dashboard_model.py`).

## Component view

```mermaid
flowchart TB
    subgraph QML ["QML"]
        main["main.qml\nlayout · dialogs · shared robot selection"]
        panels["TopBar · KpiRow · NeedsAttentionPanel\nRobotListPanel · TasksPanel"]
        mapcard["MapCard: MapPage + FleetAnalytics"]
        dialogs["NewTaskDialog · RobotControlDialog\nRegisterRobotDialog · SystemMetricsDialog"]
        theme["Theme.qml · Format.js"]
    end

    subgraph PY ["Python backend"]
        dash["Dashboard\nrefresh every dashboard.refresh_period_s"]
        dm["dashboard_model.py\npure: rows · attention · health · routes"]
        klm["KeyedListModel\nrows changed in place"]
        cfg["config.py · ui_settings.py"]
        mp["MapProvider"]
        rb["RosBridge\nfleet state · tasks · traffic"]
        ts["task_state.py\npure: ranked task state · epoch times"]
        rc["RosControl\npause · resume · speed · init_position\npending + timeout"]
        mc["MqttClient\nstate · connection · order · instantActions"]
        rr["RobotRegistry"]
        am["AdapterMetrics"]
        ws["TaskEventServer (optional)"]
        ge["GraphEditor"]
    end

    rb & mc & rc & rr & am -->|snapshots| dash
    dash --> dm
    dash --> klm
    klm -->|robots · mapRobots · attention · tasks · traffic| panels & mapcard
    dash -->|robotRows · telemetry · routes · summary| panels & mapcard & dialogs
    rb --> ts
    ws -->|taskStateUpdate| rb
    cfg --> mp & rb & rc & mc & rr & dash
    mp -->|waypoints · lanes| main
    dialogs -->|dispatch · cancel| rb
    dialogs -->|pause · speed · init_position| rc
    dialogs -->|check · register · remove| rr
    mapcard -->|closeLanes · openLanes| rb
    mapcard -->|edit · save| ge
    theme --- panels & mapcard & dialogs
```

### Refresh pipeline

The ROS executor thread and the paho thread only record what arrives: `RosBridge` keeps
the robots of each fleet and the task records, `MqttClient` the parsed state per robot and
the message log. Nothing is serialised per message.

On the GUI thread, `Dashboard.refresh()` runs every `dashboard.refresh_period_s` (0.2 s by
default). It takes one snapshot of every backend, derives what the panels show with the pure
functions of `dashboard_model.py`, and hands it over as

- `KeyedListModel`s for every list (robots, map markers, Needs Attention, tasks, traffic log):
  a row that changed emits `dataChanged` for itself only, new rows are inserted, gone rows
  removed, reordered rows moved — the model is never reset, so delegates, scroll position,
  hover and running animations survive every update;
- values (`robotRows`, `telemetry`, `routes`, the KPI summary, …) that notify only when they
  actually changed.

Needs Attention items carry a stable key (`robot:tb3_1:offline`) and the time an issue
started; the "last seen … ago" text ticks in the delegate itself. Robot markers glide to each
new pose over one refresh period. The map draws the graph (lanes, arrows, zones, editor)
and the routes on two canvases, so only the route layer is repainted as robots move.

## Key flows

### Dispatch a task

```mermaid
sequenceDiagram
    participant Q as QML dialog
    participant RB as RosBridge
    participant RMF as RMF dispatcher
    participant AD as vda5050_client_adapter

    Q->>RB: dispatch() / dispatchToRobot()
    alt ROS not connected
        RB-->>Q: dispatchResult(false, "not sent")
    else queued
        RB->>RMF: ApiRequest (dispatch_task_request\nor robot_task_request)
        RB-->>Q: dispatchResult(true, "Dispatched")
        RB->>RB: task = "queued", start timeout clock
        alt response arrives in time
            RMF-->>RB: dispatch_task_response (rmf_id, success)
            RB->>RB: task.state = accepted ? underway-track : "failed"
        else 15s pass, nothing
            RB->>RB: task.state = "failed" ("No response from dispatcher")
        end
        RMF-)AD: order over MQTT (via fleet adapter)
    end
```

`loops > 1` builds a round-trip route instead of a single leg: `dispatch()`
finds the robot's nearest waypoint via `/fleet_states` and requests
`[current_wp, destination]` with `rounds = loops`, so each round is real
travel rather than a no-op at the destination.

### Register a robot found on the broker

```mermaid
sequenceDiagram
    participant FA as Fleet adapters
    participant RR as RobotRegistry
    participant UI as QML
    actor Op as Operator

    FA-->>RR: /robot_registry, /robot_discovery (latched snapshots)
    RR->>UI: pendingJson, newRobotsDetected
    UI->>Op: toast + Needs Attention entry with Register
    Op->>UI: Register
    UI->>RR: suggestFor(fleet, robot)
    loop as the form is edited
        UI->>RR: check(request)  (dry run)
        RR->>FA: /robot_registration_requests
        FA-->>RR: /robot_registration_results (errors, warnings)
        RR->>UI: checkResult, ignored if the form has changed since
    end
    Op->>UI: Register robot (only enabled after a passing check)
    UI->>RR: register(request)
    RR->>FA: add
    FA-->>RR: result ok + new /robot_registry
    RR->>RR: robotAdded -> FleetSettings, MqttClient, RosControl, RosBridge
```

The dashboard holds no registration rules: it shows the adapter's verdict, so
a rule changed in the adapter is changed everywhere at once. Suggestions
(fleet by matching type, a name continuing the fleet's numbering, the first
free charger) only prefill the form. A request that gets no answer within
`REPLY_TIMEOUT_SEC` ends in a failure the dialog shows.

A robot removed from a fleet but still online is offered again. `removed_as`
tells the dialog its old fleet, name and charger, so it can prefill them —
registering it unchanged restores it instead of creating a new one.

### Task cancellation

```mermaid
sequenceDiagram
    participant Q as QML (Tasks table)
    participant RB as RosBridge
    participant RMF as RMF dispatcher

    Q->>RB: cancel_task(rmf_id)
    RB->>RB: task.state = "cancelled" (optimistic)
    RB->>RMF: ApiRequest (cancel_task_request)
    RMF-->>RB: task_state_update (confirms cancelled)
```

### Live telemetry (two independent sources)

```mermaid
sequenceDiagram
    participant RMF as /fleet_states
    participant Robot as Robot (MQTT state)
    participant RB as RosBridge
    participant MC as MqttClient
    participant D as Dashboard (GUI thread)
    participant UI as QML panels

    RMF->>RB: position, battery (planning value), mode, path
    Robot->>MC: speed, real battery_soc, safety, errors
    loop every dashboard.refresh_period_s
        D->>RB: robots_snapshot(), tasks_snapshot(), stale_fleets()
        D->>MC: snapshot() (+ stale flag after vda5050.state_stale_after_s)
        D->>D: display_robots: merge per field, prefer the real battery_soc
        D->>UI: changed rows only (KeyedListModel), changed values only
    end
```

### Task state from several sources

`/task_api_responses`, the websocket task events, `/dispatch_states` and the robots' task ids
in `/fleet_states` all say something about a task, in any order. `task_state.set_state` ranks
them — task events and API answers, then the dispatcher, then a robot picking up or dropping a
task id, then the dashboard's own timeouts. A better-informed source may change the state
either way; an equal or weaker one only moves it forward (queued, underway, finished). So a
late `/dispatch_states` "queued" cannot undo "underway", and "completed because the robot
dropped the task id" is replaced by RMF's "failed" when that is what happened. A finished
task gets its real end time; while it runs, the end RMF expects is shown with `~`.

## Process boundaries, at a glance

| Boundary | Crossed by | Protocol |
|:---:|---|:---:|
| UI ↔ RMF core | `/fleet_states`, `/task_api_requests`, `/task_api_responses`, `/dispatch_states`, `/lane_states`, `/lane_closure_requests`, `/rmf_traffic/negotiation_statuses`, `/dispenser_states`, `/ingestor_states` | ROS 2, domain 42 |
| UI ↔ fleet adapter | `<robot>/pause`, `<robot>/resume` (services), `speed_limit.<robot>` (param), `<robot>/init_position` (+ `_result`) | ROS 2, domain 42 |
| UI ↔ fleet adapter (metrics) | `/<adapter node>/metrics` (JSON in `std_msgs/String`, one report per `vda5050.metrics_period_s`) | ROS 2, domain 42 |
| UI ↔ fleet adapter (registration) | `/robot_registry`, `/robot_discovery`, `/robot_registration_requests`, `/robot_registration_results` (JSON in `std_msgs/String`) | ROS 2, domain 42 |
| UI ↔ robot | `<interface>/v2/<mfr>/<serial>/state`, `.../connection` | MQTT (Mosquitto) |
| Fleet adapter ↔ robot | VDA5050 `order`, `state`, `connection`, `instantActions` | MQTT (Mosquitto) |
| Fleet adapter → UI | `task_state_update`, `task_log_update` | WebSocket, optional |
| Bridge ↔ Nav2 | `NavigateToPose` action, `/odom` | ROS 2, on-robot |

Everything under "UI ↔ RMF core" and "UI ↔ fleet adapter" only works because
the UI and the fleet adapter share `ROS_DOMAIN_ID=42` — the UI process is
just another node on that graph, not a separate integration layer.
