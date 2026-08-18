# Option B — Detailed Architecture with Data Flow

Paste từng diagram vào https://mermaid.live để render.

## 1. Detailed Component Architecture + Data Flow

```mermaid
flowchart TB
    %% ============ TASK ORIGIN ============
    user(("User / API\n(dispatch patrol wp6)"))

    %% ============ OPEN-RMF CORE ============
    subgraph rmf_core ["Open-RMF Core"]
        task_api["/task_api_requests\n(rmf_task_msgs/ApiRequest)\nJSON: dispatch_task_request"]
        dispatcher["rmf_task_dispatcher\n· receives task request\n· runs cost-based bidding\n· selects lowest-cost fleet+robot"]
        schedule["rmf_traffic_schedule\n· spatiotemporal itinerary DB\n· conflict detection\n· negotiation protocol"]
        fleet_handle["rmf_fleet_adapter\n· FleetUpdateHandle\n· EasyFullControl API\n· from_config_files()"]
        fleet_state["/fleet_states\n(rmf_fleet_msgs/FleetState)\nrobot position · battery · task_id"]

        task_api -->|"1. task request"| dispatcher
        dispatcher -->|"2. bid request"| fleet_handle
        fleet_handle -->|"3. bid response\n(cost estimate)"| dispatcher
        dispatcher -->|"4. dispatch winner\ntask assigned"| fleet_handle
        fleet_handle -->|"5. register itinerary"| schedule
        fleet_handle -->|"publish"| fleet_state
    end

    user -->|"ROS 2 / REST API"| task_api

    %% ============ VDA5050 FLEET ADAPTER ============
    subgraph vda_adapter ["vda5050_fleet_adapter"]
        main_node["main.cpp\n· Adapter::make()\n· add_easy_fleet(from_config_files)\n· update loop (5 Hz)"]

        subgraph robot_layer ["Per-Robot Layer"]
            r_adapter["RobotAdapter\n· RobotCallbacks\n  { navigate, stop, action }\n· holds EasyRobotUpdateHandle"]
            r_sm["RobotStateMachine\n· IDLE / NAVIGATING /\n  EXECUTING_ACTION\n· holds CommandExecution\n· fires finished() on completion"]
        end

        subgraph shared_layer ["Shared Layer"]
            connector["Vda5050Connector\n· 1 paho MQTT connection\n· per-robot RobotContext\n  (cached ParsedState)"]
            proto["vda5050_protocol\n· make_order()\n· make_instant_actions()\n· ParsedState parser"]
            tf["transform\n· to_rmf(x,y,θ)\n· to_robot(x,y,θ)\n· 2D affine per robot"]
        end

        main_node <-->|"6a. navigate(Destination)\n6b. stop()\n6c. action(category)"| r_adapter
        r_adapter --> r_sm
        r_adapter --> connector
        connector --> proto
        connector --> tf

        main_node -->|"11. update(RobotState)\n    position · battery · activity"| fleet_handle
    end

    fleet_handle <-->|"EasyFullControl\ncallbacks + update handle"| main_node

    %% ============ MQTT ============
    subgraph mqtt ["MQTT Broker (Mosquitto)"]
        mqtt_b[("localhost:1883")]
    end

    connector -->|"7a. TB3/v2/.../order\n    JSON: 2-node order\n    (start pose → destination)\n\n7b. TB3/v2/.../instantActions\n    JSON: cancelOrder /\n    startPause / stopPause"| mqtt_b

    mqtt_b -->|"10. TB3/v2/.../state\n    JSON: agvPosition, battery,\n    orderId, lastNodeId, driving,\n    nodeStates, edgeStates,\n    actionStates, errors\n\n    TB3/v2/.../visualization\n    JSON: agvPosition (1 Hz)\n\n    TB3/v2/.../connection\n    JSON: ONLINE / OFFLINE"| connector

    %% ============ VDA5050 CLIENT ADAPTER ============
    subgraph client ["vda5050_client_adapter (TB3 · Humble)"]
        c_mqtt["MqttClient\n· paho async\n· auto reconnect\n· QoS 0/1"]
        c_node["VDA5050Node\n· MQTT ↔ ROS 2 wiring\n· state assembly + publish\n· 30s periodic + on-change"]
        c_sm["AdapterStateMachine\n· IDLE → ORDER_ACTIVE\n· CANCELLING / PAUSED\n· control action confirmation"]
        c_order["OrderManager\n· validate + accept order\n· base / horizon tracking\n· stitch validation\n· route consumption"]
        c_action["ActionManager\n· NONE / SOFT / HARD blocking\n· per-action lifecycle\n· instant action dispatch"]

        c_mqtt --> c_node
        c_node --> c_sm
        c_node --> c_order
        c_node --> c_action
    end

    mqtt_b <-->|"VDA5050 JSON\n(6 topic types)"| c_mqtt

    %% ============ TB3 BRIDGE ============
    subgraph bridge ["tb3_vda5050_bridge (TB3 · Humble)"]
        b_node["BridgeNode\n· ROS orchestration\n· Nav2 action client\n· odom → agv_position\n· battery → VDA5050 battery"]
        b_session["OrderSession\n· plan_next_work()\n· node cursor tracking\n· skip-in-place detection\n· traversal event generation"]
        b_sm["BridgeStateMachine\n· IDLE / DISPATCHING\n· NAVIGATING / PAUSED\n· WAITING_FOR_RELEASE\n· derives driving/paused"]

        b_node --> b_session
        b_node --> b_sm
    end

    c_node <-->|"8. ROS 2 Topics (vda5050_msgs)\n  ⬇ ~/order  ~/action_execute  ~/action_cancel\n  ⬆ ~/node_reached  ~/edge_entered\n     ~/edge_completed  ~/driving  ~/paused\n     ~/agv_position  ~/battery_state"| b_node

    %% ============ NAV2 ============
    subgraph nav2 ["Nav2 Stack (TB3 · Humble)"]
        nav_server["NavigateToPose\nAction Server\n· global planner\n· local planner (DWB)\n· costmap · recovery"]
        amcl["AMCL\n· laser localization\n· /amcl_pose"]
    end

    b_node <-->|"9. nav2_msgs/action\n  ⬇ NavigateToPose Goal\n     (x, y, θ in map frame)\n  ⬆ Result: SUCCEEDED /\n     ABORTED / CANCELED"| nav_server

    %% ============ TB3 HARDWARE ============
    subgraph hw ["TurtleBot3 Burger"]
        lidar["LDS-01 LiDAR\n360° laser scan"]
        opencr["OpenCR Board\nIMU · motor driver\nbattery monitoring"]
        odom_hw["Wheel Encoders\n→ /odom"]
    end

    nav_server <--> amcl
    amcl <-->|"/scan"| lidar
    nav_server <-->|"/cmd_vel"| opencr
    b_node <-->|"/odom\n/battery_state"| odom_hw
    odom_hw --- opencr
```

## 2. Task Dispatch Sequence (numbered flow)

```mermaid
sequenceDiagram
    actor User
    participant API as /task_api_requests
    participant Disp as rmf_task_dispatcher
    participant Fleet as rmf_fleet_adapter<br/>(EasyFullControl)
    participant Adapter as vda5050_fleet_adapter<br/>(RobotAdapter + SM)
    participant Conn as Vda5050Connector
    participant MQTT as MQTT Broker
    participant Client as vda5050_client_adapter
    participant Bridge as tb3_vda5050_bridge
    participant Nav2 as NavigateToPose

    User->>API: dispatch_task_request<br/>{category: patrol, place: wp6}
    API->>Disp: task request
    Disp->>Fleet: BidRequest (which robot?)
    Fleet->>Disp: BidResponse (tb3_1, cost=X)
    Disp->>Fleet: DispatchRequest → tb3_1

    rect rgb(230, 245, 255)
        Note over Fleet,Adapter: EasyFullControl callback
        Fleet->>Adapter: navigate(Destination: wp6)
        Adapter->>Adapter: StateMachine: IDLE → NAVIGATING
        Adapter->>Conn: navigate(wp6, transform to robot frame)
        Conn->>Conn: build 2-node VDA5050 order<br/>(node0: current pose, node1: wp6)
        Conn->>MQTT: publish .../order (JSON)
    end

    rect rgb(255, 245, 230)
        Note over MQTT,Nav2: Robot-side execution
        MQTT->>Client: .../order received
        Client->>Client: OrderManager: validate + accept
        Client->>Bridge: ~/order (ROS 2 vda5050_msgs)
        Bridge->>Bridge: OrderSession: plan_next_work()<br/>skip node0 (in-place) → dispatch node1
        Bridge->>Nav2: NavigateToPose Goal (wp6 coords)
        Nav2-->>Bridge: goal accepted
    end

    rect rgb(230, 255, 230)
        Note over Nav2,User: Navigation + state feedback
        Nav2-->>Bridge: result: SUCCEEDED
        Bridge->>Client: ~/node_reached (wp6)<br/>~/edge_completed
        Client->>Client: OrderManager: route consumed
        Client->>MQTT: publish .../state<br/>(lastNodeId=wp6, driving=false)
        MQTT->>Conn: .../state
        Conn->>Conn: ParsedState: is_command_completed() = true
        Conn->>Adapter: StateMachine: finished()
        Adapter->>Adapter: NAVIGATING → IDLE
        Adapter->>Fleet: execution.finished()
        Fleet->>Disp: task completed
    end
```

## 3. Cancel + Replace Order Flow (the race we fixed)

```mermaid
sequenceDiagram
    participant Fleet as EasyFullControl
    participant Adapter as vda5050_fleet_adapter
    participant MQTT as MQTT Broker
    participant Client as vda5050_client_adapter
    participant Bridge as tb3_vda5050_bridge
    participant Nav2 as Nav2

    Note over Fleet,Nav2: Robot navigating to wp6 (order A)

    Fleet->>Adapter: stop() — cancel current task
    Adapter->>MQTT: .../instantActions<br/>{cancelOrder, orderId: A}

    Fleet->>Adapter: navigate(wp4) — new task
    Adapter->>MQTT: .../order (order B: → wp4)

    rect rgb(255, 230, 230)
        Note over Client: Client adapter receives both
        MQTT->>Client: instantActions (cancelOrder A)
        Client->>Client: OrderManager: cancel order A
        Client->>Client: take_pending_cancel()<br/>resolve if new order supersedes
        MQTT->>Client: order B accepted
        Client->>Bridge: ~/order (order B)
    end

    rect rgb(230, 255, 230)
        Note over Bridge: Bridge: preempt, don't cancel+resend
        Bridge->>Bridge: invalidate old token<br/>(ignore old goal result)
        Bridge->>Nav2: NavigateToPose Goal (wp4)<br/>Nav2 auto-preempts old goal
        Nav2-->>Bridge: old goal: CANCELED (ignored, stale token)
        Nav2-->>Bridge: new goal: SUCCEEDED
        Bridge->>Client: ~/node_reached (wp4)
    end
```
