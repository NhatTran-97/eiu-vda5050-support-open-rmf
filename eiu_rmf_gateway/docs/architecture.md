# EIU RMF Gateway — Architecture

This document describes how `eiu_rmf_gateway` connects the EIU web dashboard to Open-RMF and the VDA5050 fleet adapters. The message formats and Redis keys are defined in [contract.md](contract.md); the parameters and ROS 2 interfaces are listed in the [README](../README.md).

## System context

The gateway is the only ROS 2 node of the web stack. The web backend has no ROS dependency: it writes commands to Redis and reads events and state from Redis. The gateway carries the commands to Open-RMF and the fleet adapters, and writes their answers and states back to Redis.

```mermaid
flowchart LR
    subgraph WEB ["Web dashboard (no ROS)"]
        FE["Frontend (React)\nbrowser"]
        BE["Backend (FastAPI) :8000\nservice · operations · rmf/client.py"]
        FE -->|"REST /api/v1\nWebSocket /ws"| BE
    end

    R[("Redis :6379, prefix eiu:rmf\ncommands · events (streams)\nstate hashes · nav_graph · heartbeat")]

    BE -->|"XADD commands"| R
    R -->|"XREAD events\nHGETALL · GET"| BE

    GW["eiu_rmf_gateway\n(ROS 2 node)"]
    R -->|"XREADGROUP gateway"| GW
    GW -->|"XADD events\nHSET · SET"| R

    subgraph RMF ["Open-RMF (same ROS_DOMAIN_ID)"]
        D["rmf_task_dispatcher"]
        WC["dispensers · ingestors"]
    end
    FA["vda5050_fleet_adapter_full_control"]

    GW <-->|"task API"| D
    D <-->|"bidding · task assignment"| FA
    GW <-->|"topics · services · parameters"| FA
    WC -->|"workcell states"| GW
    FA -.->|"task_state_update\n(ui_websocket_uri)"| GW
    GW <-->|"read · write"| NG[("nav graph YAML")]
    NG -->|"read at start"| FA

    FA <-->|"MQTT VDA5050\norder · instantActions · state"| ROBOT["Robots\nvda5050_client_adapter + Nav2"]
```

| Layer | Component | Transport to the next layer |
|---|---|---|
| Presentation | Frontend (React) | REST and WebSocket to the backend |
| Application | `eiu_web_backend` (users, permissions, task records, audit) | Redis streams and keys |
| Integration | `eiu_rmf_gateway` | ROS 2 topics, services, parameters; WebSocket for task events; nav graph file |
| Orchestration | Open-RMF task dispatcher, traffic schedule, fleet adapter | MQTT (VDA5050) |
| Robot | `vda5050_client_adapter`, `tb3_vda5050_bridge`, Nav2 | — |

Redis decouples the backend from ROS 2: the backend and the gateway restart independently, and a command written while the gateway is down is executed when it comes back, unless it is older than `command_max_age_s`.

## Gateway internals

```mermaid
flowchart LR
    R[("Redis")]
    subgraph GW ["eiu_rmf_gateway"]
        subgraph ASYNC ["asyncio thread (main.py)"]
            CMD["commands loop\nGateway.handle_commands"]
            FL["flush loop\nGateway.flush"]
            GWATCH["nav graph watch"]
            TE["task_events.py\nWebSocket server"]
        end
        REC["Recorder (core.py)\nevents · changed hash fields · values"]
        subgraph ROSTH ["ROS thread (SingleThreadedExecutor)"]
            ROS["ros_io.py RosIO\nsubscriptions · publishers\nrobot command queue · discovery"]
        end
        CON["contract.py\ncommand checks"]
        NGM["nav_graph.py"]
    end
    R -->|"XREADGROUP"| CMD
    CMD --> CON
    CMD -->|"task, lane, registration:\npublish directly"| ROS
    CMD -->|"robot commands:\nqueue + guard condition"| ROS
    CMD -->|"nav_graph_save"| NGM
    ROS -->|"callbacks"| REC
    TE -->|"task_state"| REC
    GWATCH --> NGM --> REC
    CMD -->|"refusals, results"| REC
    REC --> FL
    FL -->|"XADD · HSET · SET (pipeline)"| R
```

| Module | Thread | Role |
|---|---|---|
| `main.py` | both | Starts the ROS executor on its own thread and the asyncio loops: commands, flushes, nav graph watch, task events server |
| `contract.py` | asyncio | Keys, message version, command types and body checks; no ROS |
| `core.py` | asyncio | `Gateway` reads, checks, executes and acknowledges commands, and flushes; `Recorder` holds data between flushes |
| `ros_io.py` | ROS | Subscriptions, publishers, discovery of adapters and robot controls, service and parameter calls of the robot commands |
| `task_events.py` | asyncio | WebSocket server that receives `task_state_update` from the fleet adapters |
| `nav_graph.py` | asyncio | Reads the nav graph with its SHA-256, checks an edited graph, saves it with a stale check, a `.bak` copy and an atomic replace |

ROS callbacks only record data in the `Recorder`. Every `flush_period_s` the flush loop writes, in one Redis pipeline, the events in arrival order, the hash fields that changed since the last flush, the changed values (`nav_graph`) and the heartbeat with expiry `heartbeat_ttl_s`.

## Command path

Every command is read from the stream with the consumer group `gateway`, checked, executed and acknowledged (`XACK`). After a restart the gateway first reads its pending entries, then new ones.

```mermaid
flowchart TD
    A["XREADGROUP commands"] --> B{"check_command\nJSON · id · v · type · body · age"}
    B -->|"refused"| E1["command_result ok=false\nbad_json · bad_version · unknown_type\nbad_body · expired"]
    B -->|"valid"| C{"type"}
    C -->|"task_request\nlane_request"| P["publish on ROS"] --> OK["command_result ok=true"]
    C -->|"registration_request"| RG{"adapter subscribed?"}
    RG -->|"no"| E2["command_result no_adapter"]
    RG -->|"yes"| P2["publish /robot_registration_requests"] --> OK
    C -->|"robot_pause · robot_resume\nrobot_speed_limit · robot_init_position"| RB{"robot has controls?"}
    RB -->|"no"| E3["command_result unknown_robot"]
    RB -->|"yes"| Q["queue for the ROS thread"] --> LATE["command_result later:\nservice / parameter / init_position_result\nor no_answer after robot_command_timeout_s"]
    C -->|"nav_graph_save"| NGS["nav_graph.save\nstale check · .bak · atomic replace"] --> OK2["command_result ok, or stale ·\nbad_yaml · no_levels · bad_level · bad_lane"]
    E1 & E2 & E3 & OK & LATE & OK2 --> X["XACK"]
```

| Command | ROS 2 interface used | Answer to the backend |
|---|---|---|
| `task_request` (`dispatch_task_request`, `robot_task_request`, `cancel_task_request`, `interrupt_task_request`, `resume_task_request`) | publish `/task_api_requests`, `request_id` = command id | `command_result` on publish, then `task_api_response` from `/task_api_responses` |
| `robot_pause`, `robot_resume` | service `/<adapter>/<robot>/pause` or `resume` (`std_srvs/Trigger`) | `command_result` with the service response |
| `robot_speed_limit` | parameter `speed_limit.<robot>` of the adapter node | `command_result` with the parameter result |
| `robot_init_position` | publish `/<adapter>/<robot>/init_position` | `command_result` from `/<adapter>/<robot>/init_position_result` |
| `registration_request` | publish `/robot_registration_requests` | `registration_result` from `/robot_registration_results` |
| `lane_request` | publish `/lane_closure_requests` | `command_result`; new state in the `lanes` hash from `/lane_states` |
| `nav_graph_save` | none (file) | `command_result`; new `nav_graph` value |

The fleet adapter decides every operator request (registration rules, lane validity, control limits). The gateway checks only the message shape and forwards the adapter's verdict.

## State path

| Source (ROS 2 or WebSocket) | Redis key | Write |
|---|---|---|
| `/fleet_states` | `fleets` | hash field per fleet |
| `/dispenser_states`, `/ingestor_states` | `workcells` | hash field per guid |
| ROS graph scan (`/<node>/<robot>/pause`) | `adapters`, `controls` | hash field per adapter node, per robot |
| `/<adapter>/metrics` | `metrics` | hash field per adapter node |
| `/lane_states` | `lanes` | hash field per fleet |
| `/robot_registry` | `registry` | hash field per fleet |
| `/robot_discovery` | `discovery` | hash field per reporter |
| `/task_api_responses` | `events` | `task_api_response` |
| `/dispatch_states` | `events` | `dispatch_states` |
| `task_state_update` on the WebSocket | `events` | `task_state` |
| `/robot_registration_results` | `events` | `registration_result` |
| nav graph file (watched every 2 s) | `nav_graph` | value |
| gateway | `gateway` | heartbeat with expiry |

The backend reads the events with `XREAD` from its own saved cursor and the state with `HGETALL` and `GET`. It reports the link as `online` (heartbeat and at least one fleet), `offline` (heartbeat, no fleet) or `unavailable` (no heartbeat or no Redis); new tasks are refused while it is `unavailable`.

## Sequences

### Create a task

```mermaid
sequenceDiagram
    participant FE as Frontend
    participant BE as Backend
    participant R as Redis
    participant GW as Gateway
    participant D as RMF dispatcher
    participant FA as Fleet adapter
    FE->>BE: POST /api/v1/tasks
    BE->>R: XADD commands {type: task_request, body: dispatch_task_request}
    R->>GW: XREADGROUP
    GW->>D: /task_api_requests (JSON unchanged)
    GW->>R: XADD events command_result
    D-->>GW: /task_api_responses
    GW->>R: XADD events task_api_response
    D->>FA: assign the task to the winning robot
    D-->>GW: /dispatch_states
    FA-->>GW: /fleet_states, task_state_update
    GW->>R: XADD events, HSET fleets
    R-->>BE: XREAD events, HGETALL fleets
    BE-->>FE: WebSocket /ws update
```

Cancelling, pausing (interrupt) and resuming a task follow the same path with `cancel_task_request`, `interrupt_task_request` and `resume_task_request`.

### Pause a robot

```mermaid
sequenceDiagram
    participant BE as Backend
    participant R as Redis
    participant GW as Gateway (asyncio)
    participant ROS as Gateway (ROS thread)
    participant FA as Fleet adapter
    BE->>R: XADD commands {type: robot_pause, body: {robot}}
    R->>GW: XREADGROUP
    GW->>ROS: queue + guard condition
    GW->>R: XACK
    ROS->>FA: /<adapter>/<robot>/pause (Trigger)
    FA-->>ROS: success, message
    ROS->>R: (next flush) command_result, controls.paused
    R-->>BE: XREAD events
```

The adapter does not publish the pause state on ROS 2, so the gateway stores the result of the last successful pause or resume in `controls.paused`.

### Register a robot at runtime

```mermaid
sequenceDiagram
    participant BE as Backend
    participant R as Redis
    participant GW as Gateway
    participant FA as Fleet adapter
    BE->>R: XADD commands {type: registration_request, body: {request}}
    R->>GW: XREADGROUP
    GW->>FA: /robot_registration_requests (request_id = command id)
    FA-->>GW: /robot_registration_results
    FA-->>GW: /robot_registry (latched)
    GW->>R: XADD events registration_result, HSET registry
    R-->>BE: XREAD events, HGETALL registry
```

## Deployment

All ROS 2 processes run in the Jazzy container `rmf_jazzy_vda_dev` with the same `ROS_DOMAIN_ID`. Redis and the backend run in the same container or on the same host and listen on `127.0.0.1`.

```mermaid
flowchart LR
    subgraph HOST ["Host"]
        BR["Browser"]
        VITE["Vite dev server :5173\nproxy /api, /ws"]
        MQ["MQTT broker"]
        subgraph C ["Container rmf_jazzy_vda_dev (network host)"]
            BE["Backend :8000"]
            RD[("Redis :6379")]
            GW["eiu_rmf_gateway\nWebSocket :8100"]
            RMF["Open-RMF core"]
            FA["Fleet adapter"]
        end
    end
    ROBOT["Robots"]
    BR --> VITE --> BE --> RD <--> GW <--> RMF
    GW <--> FA
    FA <--> MQ <--> ROBOT
```

| Setting | Where | Value to match |
|---|---|---|
| Redis address and prefix | gateway `redis_url`, `redis_prefix`; backend `settings.yaml` | same server and prefix |
| Task events | adapter `vda5050.ui_websocket_uri`; gateway `task_events_host`, `task_events_port` | `ws://127.0.0.1:8100` |
| Nav graph | gateway `nav_graph_path`; adapter `nav_graph` launch argument; backend `map.nav_graph` | same file |
| ROS domain | `ROS_DOMAIN_ID` of the gateway, RMF and the adapters | same value |

Without `ui_websocket_uri` in the adapter config, the backend still receives `dispatch_states` and `task_api_response`, but no `task_state` events with the phase progress of a task. A fleet adapter sends task events to one address only.

Run one gateway per ROS domain and Redis prefix. The ROS 2 topics and services of the adapter and RMF's task API are open to every node in the same domain; use a private network or SROS2.
