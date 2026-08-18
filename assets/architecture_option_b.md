# Option B — Detailed Architecture Diagram

Render file này trong VS Code preview (Ctrl+Shift+V) hoặc paste vào https://mermaid.live để xem diagram.

## Full System Architecture (Open-RMF Core → TB3)

```mermaid
flowchart TB
    subgraph rmf_core ["Open-RMF Core (ROS 2 Jazzy · Domain 7)"]
        schedule("rmf_traffic_schedule\nSpatiotemporal traffic scheduling\nconflict detection + negotiation")
        dispatcher("rmf_task_dispatcher\nCost-based task bidding\ntask → fleet assignment")
        fleet_api("rmf_fleet_adapter\nEasyFullControl API\nfrom_config_files()")

        dispatcher -->|"BidRequest / DispatchRequest"| fleet_api
        fleet_api -->|"BidResponse / FleetState"| dispatcher
        schedule <-->|"itinerary updates\nconflict queries"| fleet_api
    end

    subgraph vda_adapter ["vda5050_fleet_adapter (C++ · EasyFullControl)"]
        main_cpp("main.cpp\nAdapter::make · add_easy_fleet\nupdate loop (N Hz)")

        subgraph per_robot ["Per-Robot Components"]
            robot_adapter("RobotAdapter\nRobotCallbacks: navigate / stop / action\nEasyRobotUpdateHandle::update(state)")
            state_machine("RobotStateMachine\nIDLE → NAVIGATING → IDLE\nIDLE → EXECUTING_ACTION → IDLE\nholds CommandExecution · fires finished()")
        end

        subgraph shared ["Shared Components"]
            connector("Vda5050Connector\n1 MQTT connection · N robots\nper-robot RobotContext cache\ndownlink: navigate / stop / execute_action\nuplink: get_data / is_command_completed")
            protocol("vda5050_protocol\nbuild order · instantActions\nparse state → ParsedState\npure · unit-testable")
            transform("transform\nRMF frame ↔ robot map frame\n2D affine · pure · unit-testable")
        end

        main_cpp <-->|"RobotCallbacks\nnavigate · stop · action"| robot_adapter
        robot_adapter -->|"owns"| state_machine
        robot_adapter -->|"uses"| connector
        connector --- protocol
        connector --- transform
    end

    fleet_api <-->|"EasyFullControl API\nadd_robot · update(RobotState)"| main_cpp

    subgraph mqtt_layer ["MQTT Broker (Mosquitto · localhost:1883)"]
        mqtt_broker[("MQTT\nBroker")]
    end

    connector -->|"⬇ order\n⬇ instantActions\n(cancelOrder · startPause · stopPause)"| mqtt_broker
    mqtt_broker -->|"⬆ state (30s + on-change)\n⬆ visualization (~1 Hz)\n⬆ connection (LWT)"| connector

    subgraph client ["vda5050_client_adapter (ROS 2 Humble · TB3)"]
        vda_node("VDA5050Node\nROS / MQTT wiring\nstate publish · side effects")
        adapter_sm("AdapterStateMachine\nIDLE · ORDER_ACTIVE · CANCELLING\nPAUSED · FAULTED\npause/resume/cancel confirmations")
        order_mgr("OrderManager\norder stitch validation\nbase / horizon tracking\nnewBaseRequest trigger")
        action_mgr("ActionManager\nNONE / SOFT / HARD blocking\nper-action lifecycle\npause / resume / cancel")
        mqtt_client("MqttClient\npaho async · reconnect · QoS")

        vda_node --> adapter_sm
        vda_node --> order_mgr
        vda_node --> action_mgr
        vda_node --> mqtt_client
        adapter_sm --> order_mgr
        adapter_sm --> action_mgr
    end

    mqtt_broker <-->|"VDA5050 JSON\n6 topic types"| mqtt_client

    subgraph bridge ["tb3_vda5050_bridge (ROS 2 Humble · TB3)"]
        bridge_node("BridgeNode\nROS orchestration\nNav2 action client\nodometry · battery conversion")
        order_session("OrderSession\norder cursor · traversal planning\nplan_next_work() algorithm\nbase / horizon rules")
        bridge_sm("BridgeStateMachine\nIDLE · DISPATCHING · NAVIGATING\nWAITING_FOR_RELEASE · PAUSED · FAULTED\ndriving / paused flags")

        bridge_node --> order_session
        bridge_node --> bridge_sm
    end

    vda_node <-->|"ROS 2 vda5050_msgs\n~/order · ~/node_reached\n~/edge_entered · ~/edge_completed\n~/driving · ~/paused\n~/action_execute · ~/action_cancel"| bridge_node

    subgraph nav2 ["Nav2 (ROS 2 Humble · TB3)"]
        navigate_to_pose("NavigateToPose\nAction Server\npath planning + execution")
    end

    bridge_node <-->|"NavigateToPose\ngoal / result / feedback"| navigate_to_pose

    subgraph tb3_hw ["TurtleBot3 Hardware"]
        sensors("Odometry · LiDAR · IMU\nBattery · OpenCR")
    end

    navigate_to_pose <--> sensors
```

## Data Flow Summary

```mermaid
flowchart LR
    subgraph downlink ["⬇ Downlink (RMF → Robot)"]
        direction LR
        d1["RMF task\ndispatch"] --> d2["EasyFullControl\nnavigate()"] --> d3["Vda5050Connector\nbuild 2-node order"] --> d4["MQTT\n.../order"] --> d5["VDA5050Node\nprocess_order"] --> d6["BridgeNode\non_order"] --> d7["Nav2\nNavigateToPose"]
    end

    subgraph uplink ["⬆ Uplink (Robot → RMF)"]
        direction LR
        u1["Nav2 goal\nsucceeded"] --> u2["BridgeNode\nnode_reached"] --> u3["VDA5050Node\nroute consumed"] --> u4["MQTT\n.../state"] --> u5["Vda5050Connector\nParsedState cache"] --> u6["RobotAdapter\nupdate(RobotState)"] --> u7["RMF\nposition + battery"]
    end
```

## VDA5050 MQTT Topics

```mermaid
flowchart LR
    MCS["vda5050_fleet_adapter\n(Master Control)"]
    AGV["vda5050_client_adapter\n(AGV / Robot)"]

    MCS -->|"order\n(navigation command)"| AGV
    MCS -->|"instantActions\n(cancel · pause · resume)"| AGV
    AGV -->|"state\n(full status, 30s + events)"| MCS
    AGV -->|"visualization\n(position only, ~1 Hz)"| MCS
    AGV -->|"connection\n(ONLINE / OFFLINE, LWT)"| MCS
    AGV -->|"factsheet\n(capabilities, retained)"| MCS
```
