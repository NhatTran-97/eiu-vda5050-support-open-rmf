"""Launch the VDA5050 fleet adapter node."""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    pkg = FindPackageShare("vda5050_fleet_adapter_full_control")

    config_file = LaunchConfiguration("config_file")
    nav_graph = LaunchConfiguration("nav_graph")
    node_name = LaunchConfiguration("node_name")

    return LaunchDescription([
        DeclareLaunchArgument(
            "config_file",
            default_value=PathJoinSubstitution([pkg, "config", "config_tb3.yaml"]),
            description="rmf_fleet + vda5050 config YAML"),
        DeclareLaunchArgument(
            "nav_graph",
            default_value=PathJoinSubstitution([pkg, "maps", "nav_graph.yaml"]),
            description="RMF navigation graph YAML"),
        DeclareLaunchArgument(
            "node_name",
            default_value="vda5050_fleet_adapter_full_control",
            description="ROS 2 node name -- must be unique when running "
                        "more than one fleet adapter instance (e.g. one "
                        "per robot type/fleet) in the same ROS domain"),
        Node(
            package="vda5050_fleet_adapter_full_control",
            executable="fleet_adapter",
            name=node_name,
            output="both",
            arguments=["-c", config_file, "-n", nav_graph],
        ),
    ])
