# vda5050_fleet_adapter

Connects Open-RMF to AGVs that speak VDA5050 2.1 over MQTT.
It uses RMF's EasyFullControl interface: RMF sends one destination at a time, and the adapter sends each destination as one VDA5050 order with two nodes (current position, destination).
Multi-node orders and horizon release are in [vda5050_fleet_adapter_full_control](../vda5050_fleet_adapter_full_control/README.md).

Diagrams and message flows: [docs/architecture.md](docs/architecture.md). Config keys: [Configuration](#configuration).

## Key features

| Feature | What it does |
|:---:|---|
| Destinations as VDA5050 orders | Each RMF destination is one order: base node at the AGV's position, end node at the destination. The edge `maxSpeed` is RMF's speed limit, lowered by the operator's speed limit |
| Order replacement | A new destination while an order runs cancels the running order first (`cancelOrder`). With `stitch_on_replan`, it is sent as an update of the running order instead (same `orderId`, next `orderUpdateId`) |
| Order validation | Orders and instant actions are checked against the factsheet and the AGV's maps before they are sent. A rejected order still cancels the order it replaces, and RMF is asked to replan after `replan_after_s` |
| Actions | `PerformAction` is sent as an instant action and followed to `FINISHED` or `FAILED` |
| Traffic hold | When RMF stops a robot, the AGV is paused (`startPause`) and its order kept. A new command resumes or replaces it. Without a command for 10 s, the order is cancelled. An operator resume during the hold does not unpause the AGV |
| Robot status to RMF | Position and battery to RMF. A robot gets tasks only when it is online, localized, in `AUTOMATIC` or `SEMIAUTOMATIC` mode, has no eStop, field violation or FATAL error, and is not paused by someone else |
| Operator controls | Per robot: pause, resume, speed limit, set position (`initPosition`). Per fleet: close and open lanes (`/lane_closure_requests`) |
| Multiple fleets | One adapter process per fleet, many robots per fleet |

## Package layout

```
config/          fleet configs (config_tb3.yaml, config_amr.yaml)
maps/            nav graph, building file
launch/          fleet_adapter.launch.py, fleet_adapters.launch.py
src/, include/   C++ adapter:
  main.cpp                 startup, fleet, lane closures, update loop
  robot_adapter            EasyFullControl callbacks, commission
  robot_state_machine      per-robot command state, traffic hold
  vda5050_connector        MQTT connection, per-robot VDA5050 state, orders and instant actions
  vda5050_protocol         VDA5050 message building and state parsing
  factsheet                factsheet parsing, order and action checks
  readiness                commission decision
  operator_interface       pause, resume, speed limit, initPosition
  transform.hpp            RMF frame <-> robot map frame
scripts/         dispatch and cancel tasks, mock robot, e2e test
test/            gtest suites
docker/          Jazzy development image
docs/            architecture.md
```

## Prerequisites

- ROS 2 Jazzy + Open-RMF (`ros-jazzy-rmf-fleet-adapter`, `rmf-traffic-ros2`)
- Paho MQTT C++: `apt install libpaho-mqttpp-dev libpaho-mqtt-dev`
- A running MQTT broker (Mosquitto)

`docker/run.sh` builds the image `rmf_jazzy_vda` with all of the above when it does not exist, and opens a shell in the container `rmf_jazzy_vda_dev` with the workspace at `/ros2_ws` (host network, `ROS_DOMAIN_ID=42`).

## Build & run

```bash
colcon build --packages-select vda5050_fleet_adapter
source install/setup.bash

ros2 run rmf_traffic_ros2 rmf_traffic_schedule &
ros2 run rmf_task_ros2 rmf_task_dispatcher &
ros2 launch vda5050_fleet_adapter fleet_adapters.launch.py
```

`fleet_adapters.launch.py` starts `tb3_fleet` (`config_tb3.yaml`, node `vda5050_fleet_adapter_tb3`) and `amr_fleet` (`config_amr.yaml`, node `vda5050_fleet_adapter_amr`). One fleet:

```bash
ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py \
    config_file:=/ros2_ws/src/vda5050_fleet_adapter/config/config_tb3.yaml \
    node_name:=vda5050_fleet_adapter_tb3
```

Each robot type needs its own fleet, because one fleet has one footprint and one set of limits. The node name is the prefix of the operator topics, for example `vda5050_fleet_adapter_tb3/tb3_1/pause`.

## Command line

| Command | Purpose |
|:---:|---|
| `ros2 launch … fleet_adapter.launch.py` | Run one fleet adapter (`config_file`, `nav_graph`, `node_name`) |
| `ros2 launch … fleet_adapters.launch.py` | Run the TB3 and AMR fleets together |
| `ros2 run … dispatch_patrol.py <places…>` / `cancel_task.py <task_id>` | Submit or cancel an RMF patrol task |
| `ros2 run … mock_mqtt_robot.py` | Fake VDA5050 AGV over MQTT; `--help` lists identity, pose and factsheet options |
| `ros2 run … run_mock_fleets.py <configs…>` | One mock AGV per robot in the configs |
| `ros2 run … test_dispatch_e2e.py` | End-to-end test: dispatch a patrol and check the order and the arrival on MQTT |

(`…` = `vda5050_fleet_adapter`)

## Configuration

Each fleet has one config file with two sections:
- `rmf_fleet:` — read by RMF (`FleetConfiguration::from_config_files`): robots, chargers, limits, footprint, battery, task capabilities.
- `vda5050:` — read by this adapter: MQTT broker, AGV identities and order settings.

List every robot in both sections with the same name. `interface_name`, `manufacturer` and `serial` must match the AGV's VDA5050 client.

**MQTT connection** (`vda5050.mqtt`)

| Key | Default | Description |
|:---:|:---:|---|
| `host`, `port` | `localhost`, 1883 | Broker address (`tcp://`) |
| `username`, `password` | — | Broker account; `null` connects without one |

**Robots** (`vda5050`)

| Key | Default | Description |
|:---:|:---:|---|
| `interface_name` | `uagv` | First level of every VDA5050 topic |
| `robots.<name>.manufacturer`, `serial` | `unknown`, robot name | Identity of the AGV in the topics |
| `robots.<name>.transform` | identity | `rotation` (rad), `scale` and `translation` `[x, y]` from the RMF frame to the robot's map frame |

**Orders** (`vda5050`)

| Key | Default | Description |
|:---:|:---:|---|
| `update_rate_hz` | 10 | Frequency of the update loop (> 0) |
| `strict_validation` | true | Reject orders and actions with hard violations. When off, violations are only logged |
| `stitch_on_replan` | false | Send a new destination as an update of the running order instead of replacing it |
| `replan_after_s` | 10 | Delay before RMF is asked to replan after an order was rejected or not published (>= 0) |

**Other**

| Key | Default | Description |
|:---:|:---:|---|
| `rmf_fleet.account_for_battery_drain` | false | When off, the battery is reported to RMF as full (SoC 1.0) |

Fixed values (not configurable):

| Value | Setting |
|:---:|:---:|
| Traffic hold before `cancelOrder` | 10 s |
| No state before a robot counts as offline | 10 s |
| `factsheetRequest`: first wait, retry wait, attempts | 5 s, 20 s, 3 |
| Wait for the answer to `initPosition` | 10 s |
| Node `allowedDeviationXY`, `allowedDeviationTheta` | 0.5 m, 3.15 rad |

To add a robot: add a charger waypoint (`is_charger`) in `maps/nav_graph.yaml`, then add the robot under `rmf_fleet.robots` and `vda5050.robots`.

## Testing without hardware

End-to-end with a mock AGV, on a local broker:

```bash
python3 src/vda5050_fleet_adapter/scripts/test_dispatch_e2e.py --host localhost \
    --interface AMR --manufacturer ROBOTIS --serial 0001 --target Patrol_C1

# Both fleets: one mock AGV per robot, one patrol per fleet
python3 src/vda5050_fleet_adapter/scripts/run_mock_fleets.py \
    config/config_amr.yaml config/config_tb3.yaml --host localhost --port 1883
python3 src/vda5050_fleet_adapter/scripts/test_dispatch_e2e.py --no-mock --host localhost \
    --config config/config_amr.yaml --config config/config_tb3.yaml \
    --fleet-target amr_fleet=Patrol_C3 --fleet-target tb3_fleet=Patrol_C1
```

The mock AGVs use the identities of the real robots, so the scripts refuse a broker other than localhost unless `--allow-remote` is given.

### Automated tests

```bash
colcon test --packages-select vda5050_fleet_adapter
```

| Test | What it checks |
|:---:|---|
| `test_protocol` | Order and instant action building, state parsing |
| `test_factsheet` | Factsheet parsing, blocking types, order and action checks |
| `test_connector` | Order replacement, stitching, validation, tracking after publish; no broker (publishing is recorded in the test) |
| `test_readiness` | Commission decision |
| `test_transform` | RMF frame <-> robot frame |
| `test_state_machine` | nodeId derivation |

`colcon test` also runs the ROS linters, configured in `CMakeLists.txt`. `flake8` uses [flake8.cfg](flake8.cfg): ament's default with double quotes allowed and lines up to 140 characters. `pep257`, `lint_cmake` and `xmllint` run with their defaults. `uncrustify`, `cpplint` and `copyright` are off. ament skips its own `cppcheck` on Jazzy (cppcheck 2.13 is too slow).

## Reviewer recommendations

| Recommendation | Status |
|:---:|---|
| Port the factsheet and per-action blocking | ✅ Done — [`factsheet.cpp`](src/factsheet.cpp), blocking type chosen in [`vda5050_connector.cpp`](src/vda5050_connector.cpp) |
| Decommission on disconnect | ✅ Done — [`readiness.cpp`](src/readiness.cpp) and [`RobotAdapter::apply_readiness`](src/robot_adapter.cpp) |
| Pause and resume instead of cancel | ✅ Done — `HOLDING` state in [`robot_state_machine.cpp`](src/robot_state_machine.cpp) |
| Port `initPosition` | ✅ Done — [`operator_interface.cpp`](src/operator_interface.cpp) |
| Lane closures, eStop, operating mode, speed override | ✅ Done — lane subscription in [`main.cpp`](src/main.cpp), eStop and mode in the commission decision, speed cap in the connector |
| Request and gate on the factsheet | ✅ Done — `factsheetRequest` polling and reject-on-missing-action gating in [`vda5050_connector.cpp`](src/vda5050_connector.cpp) (`poll`, `execute_instant_action`) |
| Severity-split order validation | ✅ Done — [`factsheet.cpp`](src/factsheet.cpp)'s `check_order`, wired into [`Vda5050Connector::navigate`](src/vda5050_connector.cpp) |
| Stitch order updates onto a replan | ✅ Done, opt-in — `stitch_on_replan` in [`Vda5050Connector::navigate`](src/vda5050_connector.cpp); off by default cancels the live order instead |
| Report a rejected order to RMF | ✅ Done — `NavigateStatus` from [`Vda5050Connector::navigate`](src/vda5050_connector.cpp); [`RobotAdapter::update`](src/robot_adapter.cpp) calls `replan()` after `replan_after_s` |
| Ignore resume during an RMF hold | ✅ Done — [`RobotStateMachine::resume`](src/robot_state_machine.cpp) sends no `stopPause` while `HOLDING` |
| Multi-node orders, horizon release | ➖ Not ported — EasyFullControl hands over one destination at a time; see `vda5050_fleet_adapter_full_control` |
