# fleet_bringup

One launch file for the fleet-management side on Legion-Pro — the counterpart to `robot_bringup` on the robot. Starts RMF core, one VDA5050 fleet adapter per robot type, the mock dispenser/ingestor (for Delivery demos), and the operator UI.

## Starts

- `rmf_traffic_ros2 rmf_traffic_schedule`
- `rmf_task_ros2 rmf_task_dispatcher`
- `vda5050_fleet_adapter_full_control` (`fleet_adapters.launch.py`): one
  process for `tb3_fleet` (`node_name=vda5050_fleet_adapter_tb3`) and one
  for `amr_fleet` (`node_name=vda5050_fleet_adapter_amr`)
- mock dispenser + ingestor (`mock_workcells.launch.py`)
- `eiu_fleet_ui` (`eiu_fleet_ui.launch.py`), which by default follows both
  fleet adapters above

## Prerequisites

- MQTT broker running, and the robot side (`vda5050_client_adapter` +
  `tb3_vda5050_bridge` + Nav2, or `mock_mqtt_robot.py`) already up
- `mutex_group_supervisor` (stock RMF)

## Run

```bash
colcon build --packages-select fleet_bringup
ros2 launch fleet_bringup fleet_bringup.launch.py
```

Skip a piece:

```bash
ros2 launch fleet_bringup fleet_bringup.launch.py use_mock_workcells:=false use_ui:=false
```

## Secure broker

`broker/setup_broker.py` generates a mosquitto setup with TLS on one port, one account per client and a VDA5050 topic ACL. The AGV list comes from the fleet adapter configs (`vda5050.robots`), plus
`--agv` for robots added at runtime.

```bash
python3 broker/setup_broker.py --out /etc/vda5050-broker \
    --config ../vda5050_fleet_adapter_full_control/config/config_tb3.yaml ../vda5050_fleet_adapter_full_control/config/config_amr.yaml \
    --host broker.local 192.168.10.97 --port 8883
mosquitto -c /etc/vda5050-broker/mosquitto.conf
```

| Output | Content |
|:---:|---|
| `certs/ca.crt` | CA certificate; clients use it as `ca_file`. Kept on reruns, so clients keep trusting the broker |
| `certs/ca.key` | CA private key (mode 600). Kept on reruns; keep it off the robots |
| `certs/server.crt`, `server.key` | Broker certificate for the `--host` names and addresses; renewed on each run |
| `passwd`, `credentials.txt` | Hashed passwords for mosquitto; plain `user:password` lines (mode 600) to hand out. Reruns keep existing passwords |
| `acl` | Topic rules per account |
| `mosquitto.conf` | TLS listener, no anonymous access, the files above |

| Account | May publish | May read |
|:---:|---|---|
| `fleet_master` (fleet adapters) | `order`, `instantActions` of every AGV | `state`, `visualization`, `connection`, `factsheet` of every AGV |
| `fleet_ui` (read-only client) | — | `state`, `visualization`, `connection`, `factsheet` of every AGV |
| `<manufacturer>_<serial>` (one AGV) | its own `state`, `visualization`, `connection`, `factsheet` | its own `order`, `instantActions` |

Client settings: fleet adapter `vda5050.mqtt.tls` + `username`/`password` (`${VAR}` reads the
environment); client adapter `mqtt.broker_url: ssl://<host>:8883`, `mqtt.tls.*`, `mqtt.username`,
`mqtt.password`.

`eiu_fleet_ui` does not support TLS yet, so it cannot connect to this broker. It also uses the MQTT account of the fleet adapter config, not `fleet_ui`.

`broker/test_setup_broker.py` starts a broker from a generated setup and checks the TLS listener, the
accounts and the ACL.
