# tb3_simulation

Simulates a fleet of TurtleBot3 Burgers in Gazebo Harmonic with a full Nav2 stack each, on ROS 2 Jazzy. Every robot runs in its own namespace behind its own VDA5050 client, so the fleet adapter talks to the simulation exactly as it would to real AGVs.

<p align="center">
  <img src="../assets/img/gazebo_simulation.png" alt="tb3_world in Gazebo, with the three Burgers at their chargers" width="95%" />
</p>

The node graph, ROS domains and startup order are documented in [docs/architecture.md](docs/architecture.md).

## Package structure

```
tb3_simulation/
├── launch/
│   ├── tb3_simulation_nav2.launch.py    Gazebo, /clock bridge, spawning, Nav2, RViz
│   └── vda5050_bridge_fleet.launch.py   VDA5050 bridge + client, per robot
├── config/
│   ├── robot_poses.yaml                 Robot names and spawn positions
│   ├── nav2_params.yaml                 Nav2 tuning, shared by all robots
│   ├── vda5050_bridge_sim*.yaml         Bridge topics, Nav2 action, pose thresholds
│   ├── vda5050_client_params_tb3_*.yaml VDA5050 identity, MQTT, factsheet
│   └── cyclonedds_sim.xml               DDS profile
├── maps/tb3_world/                      Map, world file, models
├── docker/                              Dockerfile, compose file, launcher script
└── scripts/patch_world.py               traffic-editor → Gazebo world generation
```

## Running it

The simulation runs in the `tb3-simulation` container, which uses host networking and NVIDIA passthrough for Gazebo rendering. The launcher script handles the X11 permissions and drops you into a shell; leaving that shell stops the container.

```bash
cd ~/ros2_ws/src/tb3_simulation/docker
./launch_docker.sh
```

To keep it up in the background instead:

```bash
docker compose -f ~/ros2_ws/src/tb3_simulation/docker/docker-compose.yaml up -d --force-recreate
docker exec -it tb3-simulation bash
```

`~/ros2_ws` is bind-mounted at `/home/eiu/sim_ws`, while `build/`, `install/` and `log/` are named volumes, so a rebuilt container keeps its previous build. Every shell in the container sources `/home/eiu/install`, so build into it after changing sources:

```bash
source /opt/ros/jazzy/setup.bash
cd /home/eiu/sim_ws
colcon build --packages-up-to tb3_simulation tb3_vda5050_bridge vda5050_client_adapter \
  --build-base /home/eiu/build --install-base /home/eiu/install
source /home/eiu/install/setup.bash
```

Start the simulation and navigation stacks first. With three robots, give it about 40 seconds before every lifecycle node reports `active`:

```bash
ros2 launch tb3_simulation tb3_simulation_nav2.launch.py robot_count:=3 use_gz_gui:=True
```

Then bring up the VDA5050 layer from a second shell in the same container. Until this runs, nothing publishes AGV state to MQTT and the fleet adapter sees no robots at all:

```bash
docker exec -it tb3-simulation bash
ros2 launch tb3_simulation vda5050_bridge_fleet.launch.py broker_url:=tcp://localhost:1883
```

## More robots

`robot_count:=N` spawns the first N robots listed in `config/robot_poses.yaml`; add entries there for more. Run larger fleets with `use_composition:=True` and without the GUI and RViz:

```bash
ros2 launch tb3_simulation tb3_simulation_nav2.launch.py robot_count:=10 \
  use_composition:=True use_gz_gui:=False use_rviz:=False
```

To watch the run, open the GUI once every robot has spawned, from another shell in the container. This also recovers a GUI that crashed or is missing a robot; the simulation keeps running without it.

```bash
gz sim -g
```

The VDA5050 layer covers `tb3_1..3` only; the other robots run in Gazebo and Nav2 but do not reach the fleet adapter.

## Launch arguments

`tb3_simulation_nav2.launch.py`:

| Argument | Default | |
|---|---|---|
| `robot_count` | `3` | At most the robots listed in `config/robot_poses.yaml` |
| `use_composition` | `False` | Each robot's Nav2 stack in one process |
| `use_gz_gui` | `False` | Gazebo GUI client |
| `use_rviz` | `True` | One RViz, following `tb3_1` |
| `slam` | `False` | Otherwise AMCL localises against `maps/tb3_world` |
| `use_sim_time` | `True` | |
| `map`, `params_file` | package files | Alternative map or Nav2 tuning |

`vda5050_bridge_fleet.launch.py` takes `broker_url` (default `tcp://localhost:1883`) and `robot_count` (1–3, default 3).

## Checking a run

```bash
ros2 topic info /clock      # expect exactly one publisher
ros2 topic hz /clock        # ~100 Hz
ros2 topic echo /tb3_1/amcl_pose --once
```

`/clock` is worth checking first. With `use_sim_time` enabled and nothing publishing it, every Nav2 node holds a clock frozen at zero, so none of the timeouts that normally recover a stuck goal can expire — the robot simply stops and logs nothing. Hardware runs on the wall clock and never reproduces this.
