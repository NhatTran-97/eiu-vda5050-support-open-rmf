# EIU Fleet UI — Architecture

A PySide6 + QML desktop dashboard for an Open-RMF fleet running the VDA5050
protocol. It watches RMF's own topics for fleet-level truth (position, task
state, traffic) and the robot's MQTT `state` topic for everything RMF doesn't
carry (speed, safety, battery detail, order progress) — then dispatches,
cancels, and directly controls robots through the same channels.

## Features

| Area | What it does |
|---|---|
| Live navigation map | Occupancy grid + nav-graph overlay, lane direction arrows, blocked-lane highlighting from live RMF traffic state, multi-robot markers with heading/pulse, planned-path overlay, click-to-pick a pose or waypoint |
| Fleet Command dashboard | KPI cards (system status, fleet + VDA5050-connected count, traffic status, active tasks), RMF/MQTT online indicators, critical/warning alert badges |
| Active Robots panel | Search/filter, battery, round progress, live telemetry badges (not-localized, no-recent-data, safety/eStop/fatal-error) |
| Robot Control dialog | Pause/resume, speed-limit override, re-localize (click-on-map or typed pose), direct "go to waypoint" pinned to that one robot |
| Recent Tasks table | Search/filter, underway-first sort, cancel button, dispatch confirmed synchronously with error feedback and a server-side timeout for a dispatcher that never responds |
| New Task dialog | Fleet-wide patrol dispatch (category, destination, loop count) — RMF bids it to whichever robot it picks |
| Fleet Analytics | Battery/task-distribution gauges, current task progress, live distance-since-last-node |
| Traffic awareness | Blocked lanes and active negotiation/conflict counts, read from real RMF topics, not inferred |
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
`displayRobots` in `main.qml`).

## Component view

```mermaid
flowchart TB
    subgraph QML ["QML"]
        main["main.qml\nKPI row · robot table · task table"]
        map["MapPage.qml"]
        ana["FleetAnalytics.qml"]
        newTask["NewTaskDialog.qml"]
        ctrl["RobotControlDialog.qml"]
    end

    subgraph PY ["Python backend"]
        cfg["config.py\nreads the adapter's own config.yaml"]
        mp["MapProvider\nmap.yaml + nav_graph.yaml"]
        rb["RosBridge\nfleet state · tasks · traffic"]
        rc["RosControl\npause/resume/speed/init_position"]
        mc["MqttClient\nVDA5050 state + connection"]
        vs["vda5050/state.py\npure parsing, no Qt"]
        ws["TaskEventServer\nwebsocket, optional"]
    end

    cfg --> mp & rb & rc & mc & ws

    mp  -->|imagePath · wpJson · lanesJson · laneIndexMapJson| map
    rb  -->|robotsJson · tasksJson · blockedLanes · activeConflicts| main
    rb  -->|dispatchResult ok/err| newTask
    rb  -->|dispatchResult ok/err| ctrl
    rc  -->|speedLimitsJson · commandResult| ctrl
    mc  -->|telemetryJson · robotsOnlineJson| main
    mc  --> vs
    ws  -->|taskStateUpdate| rb
    main --> map & ana
    newTask -->|dispatch| rb
    ctrl -->|dispatchToRobot| rb
    ctrl -->|pauseRobot · setSpeedLimit · initPosition| rc
```

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

### Live telemetry (two independent sources)

```mermaid
sequenceDiagram
    participant RMF as /fleet_states
    participant Robot as Robot (MQTT state)
    participant RB as RosBridge
    participant MC as MqttClient
    participant UI as main.qml (displayRobots)

    RMF->>RB: position, battery (planning value), mode, path
    Robot->>MC: speed, real battery_soc, safety, errors
    RB->>UI: robotsJson
    MC->>UI: telemetryJson (+ stale flag after 5s silence)
    UI->>UI: merge per-field, prefer telemetry's real battery_soc
```

## Process boundaries, at a glance

| Boundary | Crossed by | Protocol |
|---|---|---|
| UI ↔ RMF core | `/fleet_states`, `/task_api_requests`, `/task_api_responses`, `/dispatch_states`, `/lane_states`, `/rmf_traffic/negotiation_statuses` | ROS 2, domain 42 |
| UI ↔ fleet adapter | `<robot>/pause`, `<robot>/resume` (services), `speed_limit.<robot>` (param), `<robot>/init_position` (+ `_result`) | ROS 2, domain 42 |
| UI ↔ robot | `<interface>/v2/<mfr>/<serial>/state`, `.../connection` | MQTT (Mosquitto) |
| Fleet adapter ↔ robot | VDA5050 `order`, `state`, `connection`, `instantActions` | MQTT (Mosquitto) |
| Fleet adapter → UI | `task_state_update`, `task_log_update` | WebSocket, optional |
| Bridge ↔ Nav2 | `NavigateToPose` action, `/odom` | ROS 2, on-robot |

Everything under "UI ↔ RMF core" and "UI ↔ fleet adapter" only works because
the UI and the fleet adapter share `ROS_DOMAIN_ID=42` — the UI process is
just another node on that graph, not a separate integration layer.
