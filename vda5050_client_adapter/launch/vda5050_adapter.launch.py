"""
Launch file for the VDA5050 adapter node.

Usage:
    ros2 launch vda5050_client_adapter vda5050_adapter.launch.py
    ros2 launch vda5050_client_adapter vda5050_adapter.launch.py broker_url:=tcp://192.168.1.10:1883
    ros2 launch vda5050_client_adapter vda5050_adapter.launch.py broker_url:=tcp://192.168.10.43:1883
"""

import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


def generate_launch_description():
    pkg_share = get_package_share_directory("vda5050_client_adapter")
    default_params = os.path.join(pkg_share, "config", "vda5050_params.yaml")

    # ── Overrideable arguments ──────────────────────────────────────────────
    broker_url_arg = DeclareLaunchArgument(
        "broker_url",
        default_value="tcp://localhost:1883",
        description="MQTT broker URL (e.g. tcp://192.168.1.10:1883)",
    )
    manufacturer_arg = DeclareLaunchArgument(
        "manufacturer",
        default_value="ROBOTIS",
        description="AGV manufacturer identifier (VDA5050 §4.1)",
    )
    serial_number_arg = DeclareLaunchArgument(
        "serial_number",
        default_value="0001",
        description="AGV serial number",
    )

    params_file_arg = DeclareLaunchArgument(
        "adapter_params_file",
        default_value=default_params,
        description="Path to the ROS2 parameters YAML file",
    )

    # ── Node ────────────────────────────────────────────────────────────────
    adapter_node = Node(
        package="vda5050_client_adapter",
        executable="vda5050_client_adapter_node",
        name="vda5050_client_adapter",
        output="screen",
        parameters=[
            LaunchConfiguration("adapter_params_file"),
            {

                "mqtt.broker_url": ParameterValue(LaunchConfiguration("broker_url"), value_type=str),
                "vda5050.manufacturer": ParameterValue(LaunchConfiguration("manufacturer"), value_type=str),
                "vda5050.serial_number": ParameterValue(LaunchConfiguration("serial_number"), value_type=str),
            },
        ],
        remappings=[
            # ("~/agv_position", "/robot/localization/agv_position"),
            # ("~/battery_state", "/robot/battery"),
        ],
    )

    return LaunchDescription([
        broker_url_arg,
        manufacturer_arg,
        serial_number_arg,
        params_file_arg,
        adapter_node,
    ])
