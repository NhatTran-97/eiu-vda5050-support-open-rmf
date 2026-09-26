# tb3_simulation

`tb3_simulation` is a ROS 2 Jazzy package for testing TurtleBot3 navigation through the Open-RMF VDA5050 fleet adapter. It runs multiple TurtleBot3 Burger robots in Gazebo Harmonic. Each robot has an independent Nav2 stack, ROS namespace, and VDA5050 client.

<p align="center">
  <img src="../assets/img/gazebo_simulation.png" alt="Gazebo view of tb3_world with three TurtleBot3 Burger robots at charging stations" width="95%" />
</p>

The node graph, ROS domains, and startup order are documented in [docs/architecture.md](docs/architecture.md).

## Package structure

```text
tb3_simulation/
├── launch/
│   ├── tb3_simulation_nav2.launch.py     Gazebo, robot spawning, Nav2, and RViz
│   └── vda5050_bridge_fleet.launch.py    Per-robot VDA5050 bridge and client
├── config/
│   ├── robot_poses.yaml                  Robot names and spawn positions
│   ├── nav2_params.yaml                  Shared Nav2 parameters
│   ├── vda5050_bridge_sim*.yaml          Per-robot bridge parameters
│   ├── vda5050_client_params_tb3_*.yaml  Per-robot VDA5050 and MQTT parameters
│   └── cyclonedds_sim.xml                DDS configuration
├── maps/tb3_world/
│   ├── tb3_world.building.yaml           Traffic Editor source
│   ├── tb3_world.world                   Gazebo world
│   ├── map.yaml, map.pgm, map.png        Nav2 map
│   ├── nav_graphs/                       RMF navigation graph
│   └── models/                           Gazebo models and meshes
├── scripts/
│   └── patch_world.py                    Generated world post-processing
├── docker/
│   ├── Dockerfile                        Simulation image
│   ├── docker-compose.yaml               Container configuration
│   ├── entrypoint.sh                     Container shell initialization
│   ├── launch_docker.sh                  X11 and container launcher
│   └── cmd.txt                           Command examples
├── docs/
│   └── architecture.md                   Node graph, ROS domains, and startup order
├── CMakeLists.txt                        Build, world generation, and installation
└── package.xml                           ROS 2 package metadata and dependencies
```

## Prerequisites

Host requirements:

- Docker Engine with the Docker Compose plugin
- NVIDIA driver and NVIDIA Container Toolkit
- Active X11 session with `xhost` for Gazebo rendering

Build the Docker image:

```bash
cd ~/ros2_ws/src/tb3_simulation/docker
docker compose build
```

Start the container and open a shell:

```bash
./launch_docker.sh
```

The launcher grants X11 access, recreates `tb3-simulation`, and opens a Bash shell. Closing the shell stops and removes the container.

Run the container in the background:

```bash
docker compose up -d --force-recreate
docker exec -it tb3-simulation bash
```

## Build & run

**Build the workspace**

```bash
source /opt/ros/jazzy/setup.bash
cd /home/eiu/sim_ws
colcon build --packages-up-to tb3_simulation tb3_vda5050_bridge vda5050_client_adapter \
  --build-base /home/eiu/build --install-base /home/eiu/install
source /home/eiu/install/setup.bash
```

**Start simulation and navigation**

Start three robots with the Gazebo GUI:

```bash
ros2 launch tb3_simulation tb3_simulation_nav2.launch.py robot_count:=3 use_gz_gui:=True
```

Nav2 lifecycle activation takes approximately 40 seconds.

**More robots**

Use Nav2 composition and disable graphical clients to reduce resource usage:

```bash
ros2 launch tb3_simulation tb3_simulation_nav2.launch.py \
  robot_count:=N use_composition:=True use_gz_gui:=False use_rviz:=False
```

`N` sets the number of robots and must not exceed the entries in `config/robot_poses.yaml`.

Start the Gazebo GUI from a second shell in the same container after a short spawn delay:

```bash
docker exec -it tb3-simulation bash
gz sim -g
```

The VDA5050 layer supports `tb3_1` to `tb3_3`. Additional robots run only in Gazebo and Nav2.

**Start the VDA5050 layer**

Start the VDA5050 bridges and clients from a second shell in the same container:

```bash
docker exec -it tb3-simulation bash
ros2 launch tb3_simulation vda5050_bridge_fleet.launch.py broker_url:=tcp://localhost:1883
```

The VDA5050 layer publishes AGV state to MQTT for fleet adapter discovery.

**Start the fleet adapter**

Use `config_tb3_sim.yaml`. See [vda5050_fleet_adapter_full_control](../vda5050_fleet_adapter_full_control/README.md#gazebo-simulation) for the launch command and configuration.

## Launch arguments

`tb3_simulation_nav2.launch.py`:

| Argument | Default | Description |
|---|---|---|
| `robot_count` | `3` | Number of robots; limited by `config/robot_poses.yaml` |
| `use_composition` | `False` | `False`: separate Nav2 processes; `True`: one composed process per robot |
| `use_gz_gui` | `False` | Starts the Gazebo GUI |
| `use_rviz` | `True` | Starts one RViz instance for `tb3_1` |
| `slam` | `False` | `True`: SLAM; `False`: AMCL with the supplied map |
| `use_sim_time` | `True` | Uses the Gazebo `/clock` topic |
| `map`, `params_file` | Package files | Overrides the map or Nav2 parameter file |

`vda5050_bridge_fleet.launch.py`:

| Argument | Default | Description |
|---|---|---|
| `broker_url` | `tcp://localhost:1883` | MQTT broker URL |
| `robot_count` | `3` | Number of VDA5050 bridges and clients; valid range: 1–3 |
