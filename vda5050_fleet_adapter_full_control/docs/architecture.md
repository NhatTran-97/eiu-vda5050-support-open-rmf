# vda5050_fleet_adapter_full_control — Architecture

Bridges Open-RMF to a VDA5050 fleet over MQTT. RMF sees a normal fleet
adapter (`RobotCommandHandle`, `RobotUpdateHandle`); the robots see a
normal VDA5050 master controller.

## Features

| Area | What it does |
|---|---|
| Multi-robot fleet | One process per fleet, one `Connector` state slot + one `RobotCommandHandle` per robot; rejects a duplicate manufacturer/serial pair at startup |
| Task execution | `follow_new_path` (patrol/delivery/go_to_place), `dock` (parking/charging spots), `PerformAction` (arbitrary instant actions) |
| Task capabilities | Advertised per fleet from config: patrol, delivery, clean, plus any named instant action (e.g. `dock`) |
| Commission tracking | A robot is only offered new tasks while its VDA5050 state is fresh *and* has a usable pose — either going stale decommissions it |
| Horizon release | Optional `honor_waypoint_timing`: releases route waypoints to the AGV only as their scheduled time approaches, instead of the whole order at once |
| Operator interface | ROS services/param/topic per robot: pause, resume, speed-limit override, re-localize (`init_position`) |
| Factsheet awareness | Reads the AGV's declared speed/array-length/order-interval limits and warns before exceeding them |
| Stuck-order detection | Replans if an AGV never acknowledges a dispatched order's `orderId` within a timeout |
| Config validation | Fails fast at startup on bad MQTT settings, duplicate identities, or a nav-graph robot missing from `vda5050.robots` |

## System architecture

```mermaid
flowchart LR
    RMF["Open-RMF core\ntraffic_schedule · task_dispatcher"]

    subgraph FA ["vda5050_fleet_adapter_full_control"]
        direction TB
        RCH["RobotCommandHandle\n(per robot)"]
        CN["Connector\nMQTT <-> RMF state, per robot"]
        OI["OperatorInterface\npause/resume/speed/init_position"]
        RCH <--> CN
        OI --> CN
    end

    MQ[("Mosquitto\nVDA5050 JSON")]
    UI["eiu_fleet_ui\n(operator dashboard)"]

    subgraph ROBOT ["🤖 Robot"]
        CA["vda5050_client_adapter"]
        BR["tb3_vda5050_bridge"]
        N2["Nav2"]
        CA <--> BR
        BR --> N2
    end

    RMF <-->|FullControl API| RCH
    CN <-->|order, state, connection,\ninstantActions| MQ
    MQ <--> CA
    UI -->|pause/resume/speed_limit/\ninit_position| OI
    UI -.->|reads /fleet_states,\n/task_api_*| RMF
```

RMF never talks MQTT and the robot never talks ROS — this process is the
only thing that understands both.

## Component view

```mermaid
flowchart TB
    subgraph core ["core/"]
        main["main.cpp"]
        fa["fleet_adapter_full_control.cpp\nstartup, registration, update loop"]
        cfg["config.cpp\nconfig.yaml parsing"]
        opi["operator_interface.cpp"]
    end
    subgraph rmf_ ["rmf/"]
        conn["connector.cpp\nMQTT <-> per-robot state"]
        rch["robot_command_handle.cpp\nRMF command callbacks"]
    end
    subgraph vda_ ["vda5050/"]
        oh["order_handler.cpp"]
        sh["state_handler.cpp"]
        iah["instant_action_handler.cpp"]
        fsh["factsheet_handler.cpp"]
        mb["message_builder.cpp"]
    end
    subgraph mqtt_ ["mqtt/"]
        mc["mqtt_client.cpp\nPaho wrapper"]
    end

    main --> fa
    fa --> cfg & opi
    fa -->|creates, one per robot| rch
    rch --> conn
    opi --> conn
    conn --> mc
    conn --> oh & sh & iah & fsh & mb
```

## Key flows

### Order dispatch and progress tracking

```mermaid
sequenceDiagram
    participant RMF
    participant RCH as RobotCommandHandle
    participant CN as Connector
    participant Robot

    RMF->>RCH: follow_new_path(waypoints, estimator, done)
    RCH->>CN: navigate_route() / stop() if one was active
    CN->>Robot: order (MQTT)
    loop every update tick
        Robot-->>CN: state (lastNodeId, driving, errors...)
        CN-->>RCH: RobotData (position, battery, safety...)
        alt no usable pose
            RCH->>RCH: set_ready_for_orders(false)
            RCH->>RMF: decommissioned
        else order not yet acknowledged, timed out
            RCH->>RMF: replan()
        else progressing
            RCH->>RMF: estimator(index, eta)
        end
    end
    RCH->>RMF: done() once the AGV reaches the final node
```

### Commission state

A robot is only commissioned (eligible for new RMF tasks) while **both**
hold: its VDA5050 `state` arrived within `state_timeout_s`, and that state
carries a usable pose. Either one dropping decommissions it immediately;
both must return before it's offered work again.

## Configuration (`config.yaml`)

| Key | Default | Effect |
|---|---|---|
| `vda5050.interface_name` | — | VDA5050 topic prefix segment |
| `vda5050.mqtt.host/port/username/password` | `localhost:1883` | Broker connection |
| `vda5050.robots.<name>.manufacturer/serial` | — | Required per robot; missing entry fails startup |
| `vda5050.update_rate_hz` | 10 | RMF update-loop frequency |
| `vda5050.honor_waypoint_timing` | false | Horizon release paced by schedule instead of all-at-once |
| `vda5050.ui_websocket_uri` | — | Optional task-event broadcast to a UI |
| `account_for_battery_drain` | false | Off = report SoC 1.0 to RMF regardless of real battery |

## Process boundaries

| Boundary | Crossed by | Protocol |
|---|---|---|
| This adapter ↔ RMF core | `RobotCommandHandle` / `RobotUpdateHandle` (FullControl) | In-process RMF API |
| This adapter ↔ robot | `order`, `state`, `connection`, `instantActions`, `factsheet` | MQTT |
| This adapter ↔ eiu_fleet_ui | `<robot>/pause`, `<robot>/resume`, `speed_limit.<robot>`, `<robot>/init_position` | ROS 2, same domain |
| This adapter → eiu_fleet_ui | `task_state_update`, `task_log_update` | WebSocket, optional |
| Bridge ↔ Nav2 | `NavigateToPose`, `/odom` | ROS 2, on-robot |

Traffic negotiation and mutex-group locking (e.g. one-lane corridors) are
handled entirely by stock RMF (`rmf_traffic_schedule`,
`mutex_group_supervisor`) — this adapter just executes whatever route RMF
hands it.
