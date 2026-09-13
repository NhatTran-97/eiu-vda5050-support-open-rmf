# VDA5050 Client Adapter — Architecture

Architecture documentation for the `vda5050_client_adapter` package.

---

## 1. Package-Level View

```mermaid
flowchart LR
    MC["Master Control System\n(VDA5050 JSON over MQTT)"]
    Broker[("MQTT Broker")]
    Adapter["vda5050_client_adapter\nROS2 adapter node"]
    Msgs["vda5050_msgs\nROS2 message definitions"]
    Robot["Robot Driver\n(Nav stack + Action executors)"]

    MC -->|order\ninstantActions| Broker
    Broker -->|MQTT inbound| Adapter
    Adapter -->|state\nvisualization\nconnection\nfactsheet| Broker
    Broker -->|VDA5050 outbound| MC

    Adapter <-->|ROS2 topics| Robot
    Msgs -.-|shared interfaces| Adapter
    Msgs -.-|shared interfaces| Robot
```

---

## 2. MQTT Topics (VDA5050 §9) — 6/6 Implemented

| Topic | Direction | QoS | Retained |
|---|---|---|---|
| `.../order` | MC → AGV | 0 | No |
| `.../instantActions` | MC → AGV | 0 | No |
| `.../state` | AGV → MC | 0 | No |
| `.../visualization` | AGV → MC | 0 | No |
| `.../connection` | AGV → MC | 1 | Yes |
| `.../factsheet` | AGV → MC | 0 | Yes |

Topic pattern: `{interface_name}/v2/{manufacturer}/{serial_number}/{topic}`

`state` is published on a `vda5050.state_publish_interval` timer (default
30s) **and** immediately on every fresh `~/agv_position` update while
driving, throttled to at least `vda5050.position_publish_min_interval`
apart (default 0.2s). Without the position-triggered publish, Master
Control only ever saw the robot's position advance once per timer tick —
i.e. jump every 30s — instead of tracking it live.

---

## 3. Internal Module Structure

```mermaid
flowchart TB
    subgraph Adapter["vda5050_client_adapter package"]
        Main["main.cpp"]
        Node["VDA5050Node\nROS/MQTT wiring"]
        SM["AdapterStateMachine\nTop-level mode + control confirmations"]

        subgraph Logic["Business Logic"]
            Order["OrderManager\nOrder/base/horizon state\nStitch validation\nnewBaseRequest"]
            Action["ActionManager\nAction lifecycle\nNONE/SOFT/HARD blocking\npause_all / cancel_all"]
        end

        subgraph Data["Data Layer"]
            Types["vda5050_types.hpp\nInternal protocol model"]
            Json["json_converter.hpp\nJSON ↔ internal types"]
            Ros["ros_converters.hpp\nROS2 msgs ↔ internal types"]
        end

        subgraph Transport["Transport"]
            Mqtt["MqttClient\nPaho MQTT C++\nAsync, reconnect, QoS"]
        end

        Main --> Node
        Node --> SM
        Node --> Mqtt
        Node --> Order
        Node --> Action
        Node --> Json
        Node --> Ros
        SM --> Order
        SM --> Action
        Json --> Types
        Ros --> Types
        Order --> Types
        Action --> Types
    end
```

---

## 4. State Machine

```mermaid
stateDiagram-v2
    [*] --> INITIALIZING
    INITIALIZING --> CONNECTING: node setup complete
    CONNECTING --> IDLE: MQTT connected
    IDLE --> ORDER_ACTIVE: order accepted
    ORDER_ACTIVE --> ACTION_BLOCKED: SOFT/HARD action active
    ACTION_BLOCKED --> ORDER_ACTIVE: blocking action cleared
    ORDER_ACTIVE --> PAUSE_PENDING: startPause requested
    PAUSE_PENDING --> PAUSED: robot confirms paused
    PAUSED --> RESUME_PENDING: stopPause requested
    RESUME_PENDING --> ORDER_ACTIVE: robot confirms resumed
    ORDER_ACTIVE --> CANCELLING: cancelOrder requested
    CANCELLING --> IDLE: order inactive and robot stopped
    ORDER_ACTIVE --> IDLE: route fully consumed
    IDLE --> FAULTED: fatal error
    ORDER_ACTIVE --> FAULTED: fatal error
    PAUSED --> FAULTED: fatal error
    FAULTED --> IDLE: fatal error cleared
```

`AdapterStateMachine` is intentionally narrow:
- It owns top-level adapter mode, MQTT connectivity, effective driving suppression, fatal-error mode, and built-in pause/resume/cancel confirmations.
- `OrderManager` still owns route semantics and base/horizon progression.
- `ActionManager` still owns per-action lifecycle and blocking semantics.
- `VDA5050Node` translates ROS/MQTT callbacks into state-machine events and publish side effects.

**Superseded cancel:** a `cancelOrder` normally completes only once `!driving && !order_active`. Under EasyFullControl the fleet adapter issues `cancelOrder` and immediately follows it with the replacement order while the robot is still driving, so that condition never holds and the adapter would stay stuck in `CANCELLING`. When a new order is accepted, `take_pending_cancel()` resolves the still-pending cancel (marks the `cancelOrder` action FINISHED, "superseded by new order") and the mode recomputes to `ORDER_ACTIVE`.

---

## 5. Runtime Integration View

```mermaid
flowchart LR
    MC["Master Control"]
    Broker[("MQTT Broker")]
    Node["VDA5050Node"]
    SM["AdapterStateMachine"]
    OM["OrderManager"]
    AM["ActionManager"]
    Nav["Robot navigation stack"]
    Act["Robot action executors"]

    MC -->|Order JSON| Broker
    MC -->|InstantActions JSON| Broker
    Broker -->|order| Node
    Broker -->|instantActions| Node

    Node --> SM
    Node --> OM
    Node --> AM

    Node -->|~/order| Nav
    Node -->|~/action_execute| Act
    Node -->|~/action_cancel| Act

    Nav -->|~/node_reached| Node
    Nav -->|~/edge_entered| Node
    Nav -->|~/edge_completed| Node
    Nav -->|~/order_dropped| Node
    Nav -->|~/distance_since_last_node| Node
    Act -->|~/action_state_feedback| Node

    Nav -->|~/driving\n~/paused| Node
    Nav -->|~/agv_position\n~/velocity\n~/battery_state\n~/safety_state| Node

    Node -->|state\nconnection\nvisualization\nfactsheet| Broker
    Broker --> MC
```

---

## 6. Order Flow

```mermaid
sequenceDiagram
    participant MC as Master Control
    participant Node as VDA5050Node
    participant SM as AdapterStateMachine
    participant OM as OrderManager
    participant AM as ActionManager
    participant Robot as Robot Driver

    MC->>Node: publish Order (MQTT)
    Node->>OM: process_order(order)

    alt Accepted
        OM-->>Node: accepted=true
        Node->>SM: on_order_state_changed(true)
        Node->>AM: sync_order_actions()
        Node->>SM: on_action_blocking_changed(...)
        Node->>Robot: ~/order
        Node->>MC: publish state
    else Rejected
        OM-->>Node: accepted=false, reason
        Node->>MC: publish state (with orderError)
    end

    loop Robot traverses route
        Robot->>Node: ~/node_reached
        Node->>OM: node_reached()
        Node->>AM: on_node_reached()
        Node->>SM: sync order / blocking mode
        Node->>MC: publish state

        Robot->>Node: ~/edge_entered / ~/edge_completed
        Node->>OM: edge_completed()
        Node->>SM: sync order / blocking mode
        Node->>MC: publish state
    end
```

---

## 7. Instant Action Flow

```mermaid
sequenceDiagram
    participant MC as Master Control
    participant Node as VDA5050Node
    participant SM as AdapterStateMachine
    participant AM as ActionManager
    participant Robot as Robot Driver

    MC->>Node: publish InstantActions (MQTT)
    Node->>AM: process_instant_actions()

    alt Built-in (startPause / stopPause / cancelOrder / stateRequest)
        AM-->>Node: execute callback
        Node->>SM: request_pause/resume/cancel()
        Node->>Robot: ~/action_cancel (signal)
        Robot->>Node: ~/paused / ~/driving (confirm)
        Node->>SM: consume_ready_control_actions()
        Node->>AM: set_action_finished()
    else External action
        AM-->>Node: execute callback
        Node->>Robot: ~/action_execute
        Robot->>Node: ~/action_state_feedback
        Node->>SM: on_action_blocking_changed(...)
        Node->>AM: set_action_running/finished/failed()
    end

    Node->>MC: publish state
```

---

## 8. Factsheet Flow (VDA5050 §9.4)

```mermaid
sequenceDiagram
    participant Node as VDA5050Node
    participant MQTT as MQTT Broker
    participant MC as Master Control

    Note over Node: on startup
    Node->>Node: build_factsheet_from_params()
    Node->>MQTT: connect()
    MQTT-->>Node: on_connected
    Node->>MQTT: publish factsheet [QoS=0, retained=true]
    Node->>MQTT: publish connection ONLINE
    Node->>MQTT: publish state
    MC->>MQTT: subscribe factsheet → receives retained immediately
```

---

## 9. Data Model (vda5050_types.hpp)

| Struct | Description |
|---|---|
| `Header` | headerId, timestamp, version, manufacturer, serialNumber |
| `Order` | Navigation order: nodes, edges, actions |
| `InstantActions` | Set of immediately executed actions |
| `State` | Full robot state published to MC |
| `Visualization` | Real-time telemetry (position + velocity) |
| `Connection` | MQTT connection state |
| `Factsheet` | AGV technical specification |
| `Node` / `Edge` | Route graph elements with positions and actions |
| `Action` / `ActionState` | Action with NONE/SOFT/HARD blocking, lifecycle status |
| `TypeSpecification` | AGV kinematic, class, load capacity, navigation types |
| `PhysicalParameters` | Speed, acceleration, dimensions |
| `ProtocolFeatures` | Supported actions with scopes and blocking types |

---

## 10. JSON Schema Compliance (VDA5050 v2.1.0)

| Schema | Status |
|---|---|
| `order.schema.json` | ✅ Compliant |
| `instantActions.schema.json` | ✅ Compliant |
| `state.schema.json` | ✅ Compliant |
| `visualization.schema.json` | ✅ Compliant |
| `connection.schema.json` | ✅ Compliant |
| `factsheet.schema.json` | ✅ Compliant |

Key implementation notes:
- `maxArrayLens` uses dot-notation keys per §9.4: `"order.nodes"`, `"state.errors"`
- `std::optional<T>` custom serializer — absent fields are omitted from JSON output
- `agvActions` includes required `resultDescription` and `blockingTypes` array fields

---

## 11. OrderManager — Stitch Validation

When an order update is received, the stitch node is validated in priority order:

1. Must match the **last horizon node** (if horizon exists)
2. Must match the **last remaining base node** (if no horizon)
3. Must match the **last traversed node** (when both base and horizon are empty)

`newBaseRequest` is fired when remaining base nodes ≤ 1 AND horizon is not empty.

---

## 11b. OrderManager — Order Replacement & Stale Echoes

A **new** `orderId` (not an update to the active one) supersedes whatever is
active regardless of remaining route — this fleet issues one fresh order
per leg rather than one long stitched order, so that's the normal case, not
a reason to reject it. The only continuity check kept is that the new
order's first node matches the AGV's actual last-traversed node **by name**
(`sequence_id` isn't comparable across orders — each order's base restarts
at 0).

`apply_order()` snapshots the node/edge ids the order being replaced still
had queued (`stale_node_ids_` / `stale_edge_ids_`, overwritten each time —
only the immediately-previous order is remembered). If `node_reached()` or
`edge_entered()` then gets a report that doesn't match the current order's
front but *does* match that snapshot, it's a late echo from the order the
bridge just heard about being replaced — absorbed silently instead of
logged as `navigationOrderError`. A report matching neither is still a real
error.

---

## 11c. Bridge-Initiated Order Drop

The bridge (`tb3_vda5050_bridge`) can give up on an order on its own —
stuck-order timeout, a rejected action, an `initPosition` invalidating the
in-flight route — without going through the normal `cancelOrder` instant
action. `~/order_dropped` (`std_msgs/String`, the dropped `orderId`) tells
the adapter this happened, so it clears its own order/action tracking to
match instead of staying desynced from what the bridge actually did.

`~/distance_since_last_node` (`std_msgs/Float64`) streams the AGV's real
driven distance live as it moves, instead of only updating once per
`node_reached` — so `state.distanceSinceLastNode` tracks progress within
the current leg in real time.

`~/operating_mode` (`std_msgs/String`, already subscribed — see §9 Data
Model) is fed a real signal as of the bridge's `twist_mux` diagnostics
read: `AUTOMATIC` while Nav2/RMF drives, `MANUAL` the moment a joystick or
keyboard override outranks it. Before that, this field existed in the
protocol but had no real source and stayed `AUTOMATIC` regardless of what
was actually happening on the robot.

---

## 12. ActionManager — Blocking Semantics

| Blocking Type | Behavior |
|---|---|
| `NONE` | Runs concurrently with all other actions |
| `SOFT` | Stops driving; NONE actions still run in parallel |
| `HARD` | Pauses all running actions, runs alone; resumes others when finished |

Dispatch rules:
- HARD action running → no new actions dispatched
- `dispatch_paused_` = true → only instant actions dispatched
- Order actions → not dispatched until node/edge trigger is ready

---

## 13. Test Coverage — 116 Tests (all pass)

| Suite | Tests | Coverage |
|---|---|---|
| `test_adapter_state_machine` | 4 | Top-level mode transitions, control confirmations, fault/shutdown, pending-action supersede |
| `test_order_manager` | 36 | Accept, stitch, newBaseRequest, cancel, reject cases, zone_set_id clear, edge_entered ordering, order replacement, stale-echo absorption |
| `test_action_manager` | 30 | NONE/SOFT/HARD blocking, pause/resume/cancel, sync, status transition guard, HARD-wait timeout |
| `test_converters` | 46 | JSON round-trips, schema compliance, ROS↔internal |

---

## 14. Component Responsibilities

| Component | Responsibility |
|---|---|
| `VDA5050Node` | ROS/MQTT wiring, state publish, side effects |
| `AdapterStateMachine` | Top-level runtime mode, control-action confirmation, fault/connectivity state |
| `MqttClient` | Transport: connect, publish/subscribe, reconnect, QoS, retained |
| `OrderManager` | Protocol state: validate/stitch orders, track base/horizon |
| `ActionManager` | Action lifecycle: NONE/SOFT/HARD blocking, pause/resume/cancel |
| `json_converter.hpp` | JSON ↔ internal model (VDA5050 v2.1.0 compliant) |
| `ros_converters.hpp` | ROS2 msg ↔ internal model (bidirectional) |
| `vda5050_types.hpp` | Internal domain model (no external dependencies) |

---

## 15. Reading Order

1. [config/vda5050_params.yaml](../config/vda5050_params.yaml)
2. [include/vda5050_client_adapter/vda5050_types.hpp](../include/vda5050_client_adapter/vda5050_types.hpp)
3. [include/vda5050_client_adapter/adapter_state_machine.hpp](../include/vda5050_client_adapter/adapter_state_machine.hpp)
4. [src/vda5050_node.cpp](../src/vda5050_node.cpp)
5. [src/order_manager.cpp](../src/order_manager.cpp)
6. [src/action_manager.cpp](../src/action_manager.cpp)
7. [src/mqtt_client.cpp](../src/mqtt_client.cpp)
8. [include/vda5050_client_adapter/json_converter.hpp](../include/vda5050_client_adapter/json_converter.hpp)
9. [include/vda5050_client_adapter/ros_converters.hpp](../include/vda5050_client_adapter/ros_converters.hpp)

---

