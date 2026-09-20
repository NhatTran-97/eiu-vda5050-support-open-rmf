# vda5050_fleet_adapter_full_control — Architecture

Bridges Open-RMF to a VDA5050 fleet over MQTT. RMF sees a normal fleet
adapter (`RobotCommandHandle`, `RobotUpdateHandle`); the robots see a
normal VDA5050 master controller.

See [README.md § Features](../README.md#features) for the full features
list — kept in one place to avoid the two drifting apart.

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
    UI -->|lane_closure_requests| FA
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
        ov["order_validation.cpp\nhard / soft checks"]
        rs["route_stitch.cpp\nreplan tail onto a live order"]
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
    conn --> oh & sh & iah & fsh & mb & ov & rs
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

### Replanning a live order

```mermaid
flowchart TB
    A["RMF replans:\nfollow_new_path(new route)"] --> B{"stitch_on_replan\nand a live order?"}
    B -- no --> R
    B -- yes --> C{"AGV acknowledged the order,\nhas a valid pose, same map?"}
    C -- no --> R
    C -- yes --> D{"new route repeats the\nreleased part?"}
    D -- no --> R
    D -- yes --> S["order update on the same orderId,\nattached at the last released node\n(nothing is sent if the route is unchanged)"]
    R["replace the order:\nresume the held order with a new one,\nor cancelOrder first"]
```

`Connector::replan_route` does the check with `plan_stitch`. The new route
matches when its start repeats the released nodes the AGV has not reached
yet; up to three leading points on the lane the AGV is on, repeated turns
in place, and released nodes the new route passes straight through are
tolerated. VDA5050 cannot withdraw released nodes, so a route that changes
that part is refused with a `not stitching (<reason>)` log line and the
order is replaced instead. Nothing more is released while the AGV is held
for the replan.

### Traffic hold

An RMF `stop` does not cancel the order: `startPause` is sent and a
10 s deadline starts. A new path before the deadline is stitched onto the
order or replaces it, followed by `stopPause`. Without one, `cancelOrder`
is sent and the AGV is unpaused, unless the operator paused it. A pause
from this hold does not decommission the robot.

### Factsheet and validation

The retained `factsheet` is parsed into `ParsedFactsheet`. When none
arrives, `Connector::poll` sends `factsheetRequest` (after 5 s, then every
20 s, up to 3 times). Before an order or instant action is published:

| Severity | Condition | Effect |
|---|---|---|
| Hard | a pose is not finite | order rejected |
| Hard | `mapId` is not among the maps the AGV reports | order rejected |
| Hard | more nodes or edges than `maxArrayLens` allows | order rejected |
| Hard | a custom action is missing from the factsheet | action not sent |
| Soft | order sent inside `minOrderInterval`; a core action (`cancelOrder`, `startPause`, `stopPause`, `stateRequest`, `initPosition`, `factsheetRequest`) missing from the factsheet | warning only |

`strict_validation: false` turns the hard rejections into warnings.

### Commission state

`RobotCommandHandle` commissions a robot only while it is fresh, has a
usable pose, and `RobotData::ready_for_orders` holds (operating mode,
safety state, FATAL errors, pause) — the conditions are listed under
Commission tracking in the [README](../README.md#features). Any of them
failing decommissions it immediately through `apply_commission()`; all must
hold again before it is offered work.

## Configuration (`config_tb3.yaml` / `config_amr.yaml`)

One `rmf_fleet:` block applies its `profile`/`limits` to every robot listed
under it, so each robot type gets its own config file and its own fleet
adapter process (`config_tb3.yaml` → `tb3_fleet`, `config_amr.yaml` →
`amr_fleet`) — see [README.md](../README.md#multiple-robot-types-heterogeneous-fleets).
The keys below apply to either file.

| Key | Default | Effect |
|---|---|---|
| `vda5050.interface_name` | — | VDA5050 topic prefix segment |
| `vda5050.mqtt.host/port/username/password` | `localhost:1883` | Broker connection |
| `vda5050.robots.<name>.manufacturer/serial` | — | Required per robot; missing entry fails startup |
| `vda5050.update_rate_hz` | 10 | RMF update-loop frequency |
| `vda5050.honor_waypoint_timing` | false | Horizon release paced by schedule instead of all-at-once |
| `vda5050.stitch_on_replan` | false | Attach a replanned route to the live order as an update instead of replacing the order |
| `vda5050.strict_validation` | true | Reject hard violations before publishing; off only warns |
| `vda5050.ui_websocket_uri` | — | Optional task-event broadcast to a UI |
| `account_for_battery_drain` | false | Off = report SoC 1.0 to RMF regardless of real battery |

## Process boundaries

| Boundary | Crossed by | Protocol |
|---|---|---|
| This adapter ↔ RMF core | `RobotCommandHandle` / `RobotUpdateHandle` (FullControl) | In-process RMF API |
| This adapter ↔ robot | `order`, `state`, `connection`, `instantActions`, `factsheet` | MQTT |
| This adapter ↔ eiu_fleet_ui | `<robot>/pause`, `<robot>/resume`, `speed_limit.<robot>`, `<robot>/init_position` | ROS 2, same domain |
| This adapter ← eiu_fleet_ui | `/lane_closure_requests` (fleet-wide, not per-robot) | ROS 2, same domain |
| This adapter → eiu_fleet_ui | `task_state_update`, `task_log_update` | WebSocket, optional |
| Bridge ↔ Nav2 | `NavigateToPose`, `/odom` | ROS 2, on-robot |

Traffic negotiation and mutex-group locking (e.g. one-lane corridors) are
handled entirely by stock RMF (`rmf_traffic_schedule`,
`mutex_group_supervisor`) — this adapter just executes whatever route RMF
hands it.
