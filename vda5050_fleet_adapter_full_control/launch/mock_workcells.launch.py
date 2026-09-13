"""Launch the mock dispenser + ingestor used to demo Delivery tasks.

Override defaults if needed:
  ros2 launch vda5050_fleet_adapter_full_control mock_workcells.launch.py \
      dispenser_guid:=dispenser_a load_time:=8.0
"""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    dispenser_guid = LaunchConfiguration("dispenser_guid")
    ingestor_guid = LaunchConfiguration("ingestor_guid")
    load_time = LaunchConfiguration("load_time")
    unload_time = LaunchConfiguration("unload_time")

    return LaunchDescription([
        DeclareLaunchArgument("dispenser_guid", default_value="mock_dispenser_1"),
        DeclareLaunchArgument("ingestor_guid", default_value="mock_ingestor_1"),
        DeclareLaunchArgument("load_time", default_value="10"),
        DeclareLaunchArgument("unload_time", default_value="5"),
        Node(
            package="vda5050_fleet_adapter_full_control",
            executable="mock_dispenser.py",
            output="both",
            arguments=["--guid", dispenser_guid, "--load-time", load_time],
        ),
        Node(
            package="vda5050_fleet_adapter_full_control",
            executable="mock_ingestor.py",
            output="both",
            arguments=["--guid", ingestor_guid, "--unload-time", unload_time],
        ),
    ])
