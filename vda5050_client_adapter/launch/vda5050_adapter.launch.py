"""
Launch file for the VDA5050 adapter node.

Values come from adapter_params_file; a non-empty launch argument overrides its key.

Usage:
    ros2 launch vda5050_client_adapter vda5050_adapter.launch.py
    ros2 launch vda5050_client_adapter vda5050_adapter.launch.py broker_url:=tcp://192.168.1.10:1883
    ros2 launch vda5050_client_adapter vda5050_adapter.launch.py serial_number:=0002 client_id:=tb3_2
"""

import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node

# Launch argument -> ROS parameter it overrides when non-empty.
OVERRIDES = {
    "broker_url": "mqtt.broker_url",
    "client_id": "mqtt.client_id",
    "manufacturer": "vda5050.manufacturer",
    "serial_number": "vda5050.serial_number",
}


def launch_adapter(context):
    overrides = {}
    for argument, parameter in OVERRIDES.items():
        value = LaunchConfiguration(argument).perform(context)
        if value:
            overrides[parameter] = value

    return [Node(
        package="vda5050_client_adapter",
        executable="vda5050_client_adapter_node",
        name="vda5050_client_adapter",
        output="screen",
        parameters=[LaunchConfiguration("adapter_params_file"), overrides],
    )]


def generate_launch_description():
    pkg_share = get_package_share_directory("vda5050_client_adapter")
    default_params = os.path.join(pkg_share, "config", "vda5050_params.yaml")

    arguments = [
        DeclareLaunchArgument("broker_url", default_value="",
                              description="MQTT broker URL (e.g. tcp://192.168.1.10:1883); empty = params file"),
        DeclareLaunchArgument("client_id", default_value="",
                              description="MQTT client id, unique per robot; empty = params file"),
        DeclareLaunchArgument("manufacturer", default_value="",
                              description="AGV manufacturer identifier (VDA5050 §4.1); empty = params file"),
        DeclareLaunchArgument("serial_number", default_value="",
                              description="AGV serial number; empty = params file"),
        DeclareLaunchArgument("adapter_params_file", default_value=default_params,
                              description="Path to the ROS2 parameters YAML file"),
    ]

    return LaunchDescription(arguments + [OpaqueFunction(function=launch_adapter)])
