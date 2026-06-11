from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():

    params_file_arg = DeclareLaunchArgument(
        "params_file",
        default_value=PathJoinSubstitution([
            FindPackageShare("tb3_vda5050_bridge"),
            "config",
            "bridge_params.yaml",
        ]),
        description="Path to the bridge parameters YAML file",
    )

    bridge_node = Node(
        package="tb3_vda5050_bridge",
        executable="tb3_vda5050_bridge_node",
        name="tb3_vda5050_bridge",
        output="screen",
        parameters=[LaunchConfiguration("params_file")],
    )

    return LaunchDescription([
        params_file_arg,
        bridge_node,
    ])
