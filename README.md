# EIU VDA5050 Open-RMF Adapter

A ROS2 implementation of the VDA5050 v2.1.0 protocol for connecting Open-RMF,
MQTT-based VDA5050 communication, and a TurtleBot3/Nav2 robot stack.

The repository contains both sides of the integration:

- **Master Control side**: Open-RMF fleet adapter that creates VDA5050 orders.
- **Robot side**: VDA5050 client adapter and TurtleBot3 bridge that execute those
  orders on Nav2.

## Packages

| Package | Side | Description |
|---------|------|-------------|
| `vda5050_fleet_adapter` | Master Control side | Open-RMF fleet adapter. Registers the fleet/robot with RMF, receives RMF route assignments, converts RMF paths into VDA5050 orders, publishes those orders to MQTT, and feeds VDA5050 state back into RMF. |
| `vda5050_client_adapter` | Robot / AGV side | VDA5050 client adapter. Receives VDA5050 MQTT orders from the Master Control side, exposes them as ROS2 `vda5050_msgs` topics, collects robot feedback, and publishes VDA5050 state/visualization/connection/factsheet messages back to MQTT. |
| `tb3_vda5050_bridge` | Robot side | TurtleBot3 bridge. Converts `vda5050_client_adapter` ROS2 order topics into Nav2 `NavigateToPose` goals, and converts odometry, battery, navigation progress, and action feedback into VDA5050 ROS2 feedback topics. |
| `vda5050_msgs` | Shared | ROS2 message definitions used between `vda5050_client_adapter` and robot-side bridge/driver nodes. |

## Source / Package Layout

```text
ros2_ws/src/
  vda5050_fleet_adapter/     # Master Control side: Open-RMF -> VDA5050 MQTT
  vda5050_client_adapter/    # Robot side: VDA5050 MQTT -> ROS2 VDA5050 topics
  tb3_vda5050_bridge/        # Robot side: ROS2 VDA5050 topics -> Nav2/TB3
  vda5050_msgs/              # Shared ROS2 VDA5050 message interfaces
```

## Architecture

> **MC** = Master Control (Fleet Management System)

```
Open-RMF / RMF Dispatcher
        |
        | RMF task and route assignment
        v
vda5050_fleet_adapter                 [Master Control side]
        |
        | MQTT / VDA5050 v2.1.0
        v
MQTT Broker
        |
        | MQTT / VDA5050 v2.1.0
        v
vda5050_client_adapter                [Robot / AGV side]
        |
        | ROS2 topics / vda5050_msgs
        v
tb3_vda5050_bridge                    [Robot side]
        |
        | Nav2 NavigateToPose + telemetry
        v
TurtleBot3 / Nav2
```

In this Open-RMF integration, `vda5050_fleet_adapter` is the Master Control side.
It behaves like the fleet manager that publishes VDA5050 orders and consumes AGV
state.

`vda5050_client_adapter` and `tb3_vda5050_bridge` are robot-side components.
Together they receive the VDA5050 order, execute it on TurtleBot3/Nav2, and
publish robot state back toward the Master Control side.

The robot-side adapter exposes two interfaces:

| Interface | Protocol | Direction | Description |
|-----------|----------|-----------|-------------|
| **Northbound** | MQTT - VDA5050 v2.1.0 standard messages | Master Control side <-> `vda5050_client_adapter` | order, instantActions, state, visualization, connection, factsheet |
| **Southbound** | ROS2 topics (`vda5050_msgs`) | `vda5050_client_adapter` <-> robot bridge/driver | order dispatch, action feedback, AGV position, battery, node/edge progress |

If an external VDA5050 Master Control system is used instead of Open-RMF, it can
publish orders directly to the same MQTT broker and the robot-side
`vda5050_client_adapter` can still consume them.

## Requirements

- ROS2 Jazzy
- `libpaho-mqtt-dev` / `libpaho-mqttpp-dev`
- MQTT broker (e.g. Mosquitto)

```bash
sudo apt install -y libpaho-mqtt-dev libpaho-mqttpp-dev mosquitto mosquitto-clients jq
```

## Clone & Build

```bash
# Clone into ROS2 workspace src/
mkdir -p ~/ros2_ws/src
cd ~/ros2_ws/src
git clone https://github.com/NhatTran-97/eiu-vda5050-client-adapter.git .

# Build all packages
cd ~/ros2_ws
colcon build --packages-select \
  vda5050_msgs \
  vda5050_client_adapter \
  tb3_vda5050_bridge \
  vda5050_fleet_adapter
source install/setup.bash
```

## Run

### Master Control side: Open-RMF fleet adapter

```bash
# Start MQTT broker
sudo systemctl start mosquitto

# Run the Open-RMF VDA5050 fleet adapter
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py
```

The `vda5050_fleet_adapter` node is the Master Control side for the Open-RMF
setup. It publishes VDA5050 orders to MQTT and subscribes to robot state.

### Robot side: VDA5050 client adapter

```bash
ros2 run vda5050_client_adapter vda5050_client_adapter_node \
  --ros-args --params-file \
  ~/ros2_ws/install/vda5050_client_adapter/share/vda5050_client_adapter/config/vda5050_params.yaml
```

The `vda5050_client_adapter` node receives VDA5050 MQTT orders and republishes
them as ROS2 topics for the robot-side bridge.

### Robot side: TurtleBot3 bridge

```bash
ros2 launch tb3_vda5050_bridge bridge.launch.py
```

The `tb3_vda5050_bridge` node runs on the robot side. It converts VDA5050 ROS2
orders into Nav2 goals and publishes robot feedback back to
`vda5050_client_adapter`.

## Docker

```bash
cd ~/ros2_ws/src/vda5050_client_adapter

# Stop host mosquitto if running on port 1883
sudo systemctl stop mosquitto

# Build image + start all services (broker + adapter)
docker compose up -d --build
```

## MQTT Topics (VDA5050 v2.1.0)

Topic pattern: `{interface_name}/v2/{manufacturer}/{serial_number}/{topic}`

| Topic | Direction | Description |
|-------|-----------|-------------|
| `.../order` | MC → Robot | Navigation order |
| `.../instantActions` | MC → Robot | Instant commands (pause, cancel…) |
| `.../state` | Robot → MC | Full robot state (every 30s) |
| `.../visualization` | Robot → MC | Real-time position (every 1s) |
| `.../connection` | Robot → MC | Online/Offline (LWT) |
| `.../factsheet` | Robot → MC | Robot specifications |

## Configuration

Edit [`vda5050_client_adapter/config/vda5050_params.yaml`](vda5050_client_adapter/config/vda5050_params.yaml):

```yaml
mqtt:
  broker_url: "tcp://localhost:1883"

vda5050:
  interface_name: "TB3"
  manufacturer:   "ROBOTIS"
  serial_number:  "0001"
```

## Documentation

- [VDA5050 Fleet Adapter Architecture](vda5050_fleet_adapter/docs/architecture.md)
- [VDA5050 Client Adapter Architecture](vda5050_client_adapter/docs/architecture.md)
- [TB3 VDA5050 Bridge Architecture](tb3_vda5050_bridge/docs/architecture.md)
- [MQTT Test Guide](vda5050_client_adapter/docs/mqtt_test_guide.md)
- [Docker Guide](vda5050_client_adapter/docs/docker_guide.md)
