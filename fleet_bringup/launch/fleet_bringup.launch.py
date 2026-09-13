"""Bring up the fleet-management side: RMF core, the VDA5050 fleet adapter,
the mock dispenser/ingestor, and the operator UI -- the Legion-Pro counterpart
to robot_bringup on the robot side.

  ros2 launch fleet_bringup fleet_bringup.launch.py
  ros2 launch fleet_bringup fleet_bringup.launch.py use_mock_workcells:=false use_ui:=false
"""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    fleet_adapter_pkg = FindPackageShare("vda5050_fleet_adapter_full_control")
    use_mock_workcells = LaunchConfiguration("use_mock_workcells")
    use_ui = LaunchConfiguration("use_ui")

    return LaunchDescription([
        DeclareLaunchArgument("use_mock_workcells", default_value="true",
                              description="Launch the mock dispenser/ingestor for Delivery demos"),
        DeclareLaunchArgument("use_ui", default_value="true",
                              description="Launch eiu_fleet_ui"),

        Node(package="rmf_traffic_ros2", executable="rmf_traffic_schedule", output="both"),
        Node(package="rmf_task_ros2", executable="rmf_task_dispatcher", output="both"),

        IncludeLaunchDescription(
            PythonLaunchDescriptionSource(
                PathJoinSubstitution([fleet_adapter_pkg, "launch", "fleet_adapter.launch.py"]))),

        IncludeLaunchDescription(
            PythonLaunchDescriptionSource(
                PathJoinSubstitution([fleet_adapter_pkg, "launch", "mock_workcells.launch.py"])),
            condition=IfCondition(use_mock_workcells)),

        Node(package="eiu_fleet_ui", executable="eiu_fleet_ui", output="both",
            condition=IfCondition(use_ui)),
    ])
