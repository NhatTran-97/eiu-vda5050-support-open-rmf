from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():

    # Named specifically (not "params_file") -- DeclareLaunchArgument names are
    # shared across the whole launch tree, not scoped per include. A generic
    # name here collided with vda5050_adapter.launch.py's own "params_file"
    # when both are included from bringup_nav.launch.py: whichever file loads
    # first wins the name, so the adapter silently inherited the bridge's
    # bridge_params.yaml instead of its own vda5050_params.yaml.
    params_file_arg = DeclareLaunchArgument(
        "bridge_params_file",
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
        parameters=[LaunchConfiguration("bridge_params_file")],
    )

    return LaunchDescription([
        params_file_arg,
        bridge_node,
    ])
