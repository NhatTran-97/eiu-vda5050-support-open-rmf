# Docker Guide — VDA5050 Client Adapter

## Prerequisites

### 1. Source packages

Both packages must be present under `src/`:

```
ros2_ws/src/
├── vda5050_msgs/              ← required dependency
└── vda5050_client_adapter/   ← this package
```

### 2. Docker & Docker Compose

```bash
# Install Docker
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER   # re-login after this

# Verify
docker --version
docker compose version
```

### 3. Host tools for MQTT testing

```bash
# mosquitto clients (mosquitto_sub / mosquitto_pub)
sudo apt install -y mosquitto-clients

# jq — JSON formatter
sudo apt install -y jq
```

### 4. ROS2 (optional — for simulating robot feedback)

Only required for tests that simulate robot feedback (e.g. `startPause/stopPause`).

Set the following on the host before running `ros2` commands:

```bash
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

Then publish topics normally from the host:

```bash
ros2 topic pub /vda5050_client_adapter/paused std_msgs/msg/Bool '{data: true}' --once
```

---

## File Structure

```
vda5050_client_adapter/
├── Dockerfile
├── docker-compose.yml
├── docker-entrypoint.sh
└── docker/
    └── mosquitto/
        └── mosquitto.conf
```

---

## Build & Run

> If host mosquitto is running, stop it first to free port 1883:
> ```bash
> sudo systemctl stop mosquitto
> ```

```bash
cd ~/ros2_ws/src/vda5050_client_adapter

# Build image + start all services (mqtt-broker + vda5050-adapter)
docker compose up -d --build

# Check status
docker compose ps

# View logs
docker compose logs -f
docker compose logs -f vda5050-adapter
docker compose logs -f mqtt-broker

# Stop
docker compose down
```

> To restart without rebuilding (code unchanged):
> ```bash
> docker compose up -d
> ```

---

## Run Adapter Only (external broker)

```bash
docker run --rm \
  --network host \
  -e ROS_DOMAIN_ID=0 \
  vda5050-client-adapter:latest
```

Override broker URL at runtime:

```bash
docker run --rm \
  --network host \
  vda5050-client-adapter:latest \
  ros2 run vda5050_client_adapter vda5050_client_adapter_node \
  --ros-args -p mqtt.broker_url:=tcp://192.168.1.10:1883
```

---

## Override Config at Runtime

```bash
docker run --rm \
  --network host \
  -v $(pwd)/config/vda5050_params.yaml:/ros2_ws/install/vda5050_client_adapter/share/vda5050_client_adapter/config/vda5050_params.yaml \
  vda5050-client-adapter:latest
```

---

## Test MQTT from Host

```bash
# Subscribe to all topics
mosquitto_sub -h localhost -p 1883 -t "TB3/v2/ROBOTIS/0001/#" -v

# Trigger immediate state publish
mosquitto_pub -h localhost -p 1883 \
  -t "TB3/v2/ROBOTIS/0001/instantActions" \
  -m '{"headerId":1,"timestamp":"2026-05-27T00:00:00Z","version":"2.1.0","manufacturer":"ROBOTIS","serialNumber":"0001","actions":[{"actionId":"ia-sr-001","actionType":"stateRequest","blockingType":"NONE"}]}'
```

---

## Services Summary

| Service | Image | Port | Description |
|---------|-------|------|-------------|
| `mqtt-broker` | eclipse-mosquitto:2.0 | 1883 (MQTT), 9001 (WS) | MQTT broker |
| `vda5050-adapter` | vda5050-client-adapter:latest | — | VDA5050 adapter node |
