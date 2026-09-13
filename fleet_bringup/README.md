# fleet_bringup

One launch file for the fleet-management side on Legion-Pro — the counterpart
to `robot_bringup` on the robot. Starts RMF core, the VDA5050 fleet adapter,
the mock dispenser/ingestor (for Delivery demos), and the operator UI.

## Starts

- `rmf_traffic_ros2 rmf_traffic_schedule`
- `rmf_task_ros2 rmf_task_dispatcher`
- `vda5050_fleet_adapter_full_control` (`fleet_adapter.launch.py`)
- mock dispenser + ingestor (`mock_workcells.launch.py`)
- `eiu_fleet_ui`

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
