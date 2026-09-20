"""Launch the C++ VDA5050 EasyFullControl fleet adapter.

Override paths if needed:
  ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py \
      config_file:=/abs/config_tb3.yaml nav_graph:=/abs/nav_graph.yaml
"""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    pkg = FindPackageShare("vda5050_fleet_adapter")

    config_file = LaunchConfiguration("config_file")
    nav_graph = LaunchConfiguration("nav_graph")
    node_name = LaunchConfiguration("node_name")

    return LaunchDescription([
        DeclareLaunchArgument(
            "config_file",
            default_value=PathJoinSubstitution([pkg, "config", "config_tb3.yaml"]),
            description="EasyFullControl + vda5050 config YAML"),
        DeclareLaunchArgument(
            "nav_graph",
            default_value=PathJoinSubstitution([pkg, "maps", "nav_graph.yaml"]),
            description="RMF navigation graph YAML"),
        DeclareLaunchArgument(
            "node_name",
            default_value="vda5050_fleet_adapter",
            description="ROS node name; operator topics are <node_name>/<robot>/..."),
        Node(
            package="vda5050_fleet_adapter",
            executable="fleet_adapter",
            name=node_name,
            output="both",
            arguments=["-c", config_file, "-n", nav_graph],
        ),
    ])
