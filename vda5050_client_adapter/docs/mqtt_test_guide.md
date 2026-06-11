# MQTT Test Guide — VDA5050 Client Adapter

**Topic pattern:** `TB3/v2/ROBOTIS/0001/{topic}`  
**Rule:** `actionId` must be unique per session.

## Prerequisites

```bash
cd ~/ros2_ws/src/vda5050_client_adapter
docker compose up -d
docker compose ps   # both mqtt-broker and vda5050-adapter must be Up
```

**Host ROS2 (required for Tests 2 & 3 — Host option):**
```bash
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

---

## Test 1 — stateRequest

**Purpose:** Send a `stateRequest` instant action — adapter must respond immediately with current state, without waiting for the 30s timer.

| Step | Terminal | Role | Command |
|------|----------|------|---------|
| 1 | T1 | Client adapter output — waiting for state response | `mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/state" -C 1 \| jq '{orderId,driving,paused,operatingMode}'` |
| 2 | T2 | Simulated Fleet MC — sends `stateRequest` instant action | `mosquitto_pub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/instantActions" -m '{"headerId":1,"timestamp":"2026-05-28T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","actions":[{"actionId":"ia-sr-001","actionType":"stateRequest","blockingType":"NONE"}]}'` |

**Expected:**
```json
{ "orderId": "", "driving": false, "paused": false, "operatingMode": "AUTOMATIC" }
```

![Test 1 — stateRequest](<img/Test 1 — stateRequest.png>)

---

## Test 2 — startPause

**Purpose:** Send a `startPause` instant action — adapter must set `paused: true` after robot confirms via ROS2 topic.

| Step | Terminal | Role | Command |
|------|----------|------|---------|
| 1 | T1 | Simulated Robot — confirms pause via ROS2 *(choose one)* | **Host:** `ros2 topic pub /vda5050_client_adapter/paused std_msgs/msg/Bool '{data: true}' --once` |
|   |    |  | **Docker:** `docker exec vda5050-adapter bash -c "source /opt/ros/jazzy/setup.bash && source /ros2_ws/install/setup.bash && ros2 topic pub /vda5050_client_adapter/paused std_msgs/msg/Bool '{data: true}' --once"` |
| 2 | T2 | Client adapter output — waiting for updated state | `mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/state" -C 1 \| jq '{paused}'` |
| 3 | T3 | Simulated Fleet MC — sends `startPause` instant action | `mosquitto_pub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/instantActions" -m '{"headerId":2,"timestamp":"2026-05-28T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","actions":[{"actionId":"ia-pause-001","actionType":"startPause","blockingType":"NONE"}]}'` |

**Expected:**
```json
{ "paused": true }
```

![Test 2 — startPause](<img/Test 2 — startPause.png>)

---

## Test 3 — stopPause

**Purpose:** Send a `stopPause` instant action — adapter must set `paused: false` after robot confirms via ROS2 topic.

| Step | Terminal | Role | Command |
|------|----------|------|---------|
| 1 | T1 | Simulated Robot — confirms resume via ROS2 *(choose one)* | **Host:** `ros2 topic pub /vda5050_client_adapter/paused std_msgs/msg/Bool '{data: false}' --once` |
|   |    |  | **Docker:** `docker exec vda5050-adapter bash -c "source /opt/ros/jazzy/setup.bash && source /ros2_ws/install/setup.bash && ros2 topic pub /vda5050_client_adapter/paused std_msgs/msg/Bool '{data: false}' --once"` |
| 2 | T2 | Client adapter output — waiting for updated state | `mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/state" -C 1 \| jq '{paused}'` |
| 3 | T3 | Simulated Fleet MC — sends `stopPause` instant action | `mosquitto_pub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/instantActions" -m '{"headerId":3,"timestamp":"2026-05-28T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","actions":[{"actionId":"ia-pause-002","actionType":"stopPause","blockingType":"NONE"}]}'` |

**Expected:**
```json
{ "paused": false }
```

![Test 3 — stopPause](<img/Test 3 — stopPause.png>)

---

## Test 4 — sendOrder

**Purpose:** Send an order to the adapter — adapter must load the route and reflect it in state (`nodeStates`, `edgeStates`).

| Step | Terminal | Role | Command |
|------|----------|------|---------|
| 1 | T1 | Client adapter output — waiting for state with order info | `mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/state" -C 1 \| jq '{orderId,nodeStates:.nodeStates\|length,edgeStates:.edgeStates\|length}'` |
| 2 | T2 | Simulated Fleet MC — sends order with 2 nodes, 1 edge | `mosquitto_pub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/order" -m '{"headerId":6,"timestamp":"2026-05-28T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","orderId":"order-001","orderUpdateId":0,"nodes":[{"nodeId":"n1","sequenceId":0,"released":true,"nodePosition":{"x":0.0,"y":0.0,"mapId":"map","theta":0.0},"actions":[]},{"nodeId":"n2","sequenceId":2,"released":true,"nodePosition":{"x":1.0,"y":0.0,"mapId":"map","theta":0.0},"actions":[]}],"edges":[{"edgeId":"e1","sequenceId":1,"released":true,"startNodeId":"n1","endNodeId":"n2","actions":[]}]}'` |

**Expected:**
```json
{ "orderId": "order-001", "nodeStates": 2, "edgeStates": 1 }
```

![Test 4 — sendOrder](<img/Test 4 — sendOrder.png>)

---

## Test 5 — cancelOrder

**Purpose:** Send a `cancelOrder` instant action — adapter must reset `orderId` to empty and clear all node/edge states.

| Step | Terminal | Role | Command |
|------|----------|------|---------|
| 1 | T1 | Simulated Fleet MC — sends order `order-002` (setup) | `mosquitto_pub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/order" -m '{"headerId":6,"timestamp":"2026-05-28T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","orderId":"order-002","orderUpdateId":0,"nodes":[{"nodeId":"n1","sequenceId":0,"released":true,"nodePosition":{"x":0.0,"y":0.0,"mapId":"map","theta":0.0},"actions":[]},{"nodeId":"n2","sequenceId":2,"released":true,"nodePosition":{"x":1.0,"y":0.0,"mapId":"map","theta":0.0},"actions":[]}],"edges":[{"edgeId":"e1","sequenceId":1,"released":true,"startNodeId":"n1","endNodeId":"n2","actions":[]}]}'` |
| 2 | T2 | Client adapter output — waiting for state after cancel | `mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/state" -C 1 \| jq '{orderId,nodeStates:.nodeStates\|length,edgeStates:.edgeStates\|length}'` |
| 3 | T3 | Simulated Fleet MC — sends `cancelOrder` instant action | `mosquitto_pub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/instantActions" -m '{"headerId":7,"timestamp":"2026-05-28T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","actions":[{"actionId":"ia-cancel-002","actionType":"cancelOrder","blockingType":"NONE"}]}'` |

**Expected:**
```json
{ "orderId": "", "nodeStates": 0, "edgeStates": 0 }
```

![Test 5 — cancelOrder](<img/Test 5 — cancelOrder.png>)

---

## Test 6 — LWT (CONNECTIONBROKEN)

**Purpose:** Kill the adapter abruptly — broker automatically delivers a pre-registered disconnect message (`CONNECTIONBROKEN`) to all subscribers on the `/connection` topic.

| Step | Terminal | Role | Command |
|------|----------|------|---------|
| 1 | T1 | Client adapter output — listening on `/connection` topic | `mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/connection" \| jq .` |
| 2 | T2 | Kill adapter — simulates abrupt connection loss | `docker kill --signal=KILL vda5050-adapter` |

**Expected** (T1 receives two messages in sequence):
```json
{ "connectionState": "ONLINE" }
{ "connectionState": "CONNECTIONBROKEN" }
```

Restart adapter after test:
```bash
docker compose start vda5050-adapter
```

![Test 6 — CONNECTIONBROKEN](<img/Test 6 — CONNECTIONBROKEN.png>)

---

## Results Summary

| # | Test Case | Purpose | Result |
|---|-----------|---------|--------|
| 1 | stateRequest | On-demand state publish | ✅ Pass |
| 2 | startPause | Robot pause confirmed | ✅ Pass |
| 3 | stopPause | Robot resume confirmed | ✅ Pass |
| 4 | sendOrder | Order accepted, route loaded | ✅ Pass |
| 5 | cancelOrder | Order cancelled, state cleared | ✅ Pass |
| 6 | LWT CONNECTIONBROKEN | Abrupt disconnect detected | ✅ Pass |
