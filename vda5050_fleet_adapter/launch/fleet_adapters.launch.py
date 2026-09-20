"""Launch separate TB3 and AMR adapter nodes with their own fleet configs."""
from launch import LaunchDescription
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    """Include one adapter launch per robot fleet."""
    pkg = FindPackageShare("vda5050_fleet_adapter")
    fleet_adapter_launch = PathJoinSubstitution(
        [pkg, "launch", "fleet_adapter.launch.py"])
    nav_graph = PathJoinSubstitution([pkg, "maps", "nav_graph.yaml"])

    tb3_fleet = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(fleet_adapter_launch),
        launch_arguments={
            "config_file": PathJoinSubstitution([pkg, "config", "config_tb3.yaml"]),
            "nav_graph": nav_graph,
            "node_name": "vda5050_fleet_adapter_tb3",
        }.items(),
    )

    amr_fleet = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(fleet_adapter_launch),
        launch_arguments={
            "config_file": PathJoinSubstitution([pkg, "config", "config_amr.yaml"]),
            "nav_graph": nav_graph,
            "node_name": "vda5050_fleet_adapter_amr",
        }.items(),
    )

    return LaunchDescription([tb3_fleet, amr_fleet])
