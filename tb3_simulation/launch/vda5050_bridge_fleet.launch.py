"""Launch VDA5050 bridges and client adapters for simulated TB3 robots.

Each robot uses a separate ROS namespace and parameter files. This prevents
node, topic, and MQTT client ID conflicts.

    ros2 launch tb3_simulation vda5050_bridge_fleet.launch.py
    ros2 launch tb3_simulation vda5050_bridge_fleet.launch.py robot_count:=1
    ros2 launch tb3_simulation vda5050_bridge_fleet.launch.py broker_url:=tcp://localhost:1883
"""
import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, GroupAction, IncludeLaunchDescription
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PythonExpression
from launch_ros.actions import PushRosNamespace


# Map each robot to its VDA5050 serial number and parameter files.
# Separate client files provide unique MQTT client IDs.
ROBOTS = (
    ('tb3_1', '0001', 'vda5050_bridge_sim.yaml', 'vda5050_client_params_tb3_1.yaml'),
    ('tb3_2', '0002', 'vda5050_bridge_sim_tb3_2.yaml', 'vda5050_client_params_tb3_2.yaml'),
    ('tb3_3', '0003', 'vda5050_bridge_sim_tb3_3.yaml', 'vda5050_client_params_tb3_3.yaml'),
)


def generate_launch_description():
    tb3_sim_dir = get_package_share_directory('tb3_simulation')
    bridge_dir = get_package_share_directory('tb3_vda5050_bridge')
    adapter_dir = get_package_share_directory('vda5050_client_adapter')

    broker_url_arg = DeclareLaunchArgument(
        'broker_url', default_value='tcp://localhost:1883', description='MQTT broker shared by every simulated robot')
    robot_count_arg = DeclareLaunchArgument(
        'robot_count', default_value='3', choices=['1', '2', '3'],
        description='How many of tb3_1/tb3_2/tb3_3 to bridge (matches ' "tb3_simulation's own robot_count)")

    broker_url = LaunchConfiguration('broker_url')
    robot_count = LaunchConfiguration('robot_count')

    actions = [broker_url_arg, robot_count_arg]

    for index, (name, serial, bridge_params_filename, client_params_filename) in enumerate(ROBOTS, start=1):
        actions.append(GroupAction(condition=IfCondition( PythonExpression([str(index), ' <= ', robot_count])),
            actions=[PushRosNamespace(name),
                IncludeLaunchDescription(PythonLaunchDescriptionSource(os.path.join(bridge_dir, 'launch', 'bridge.launch.py')),
                    launch_arguments={'bridge_params_file': os.path.join(tb3_sim_dir, 'config', bridge_params_filename),}.items(),),
                IncludeLaunchDescription(PythonLaunchDescriptionSource(os.path.join(adapter_dir, 'launch', 'vda5050_adapter.launch.py')),
                    launch_arguments={
                        'broker_url': broker_url,
                        'manufacturer': 'ROBOTIS',
                        'serial_number': serial,
                        'adapter_params_file': os.path.join(tb3_sim_dir, 'config', client_params_filename),
                    }.items(),),
            ],
        ))

    return LaunchDescription(actions)
