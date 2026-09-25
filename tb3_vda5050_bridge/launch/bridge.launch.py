from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    params_file_arg = DeclareLaunchArgument(
        "bridge_params_file",
        default_value=PathJoinSubstitution([
            FindPackageShare("tb3_vda5050_bridge"),
            "config",
            "bridge_params.yaml",
        ]),
        description="Path to the bridge parameters YAML file",
    )
    use_mock_load_arg = DeclareLaunchArgument(
        "use_mock_load",
        default_value="true",
        description="Also run mock_load_publisher (fake load for delivery tests); false on robots with a real load sensor",
    )

    bridge_node = Node(
        package="tb3_vda5050_bridge",
        executable="tb3_vda5050_bridge_node",
        name="tb3_vda5050_bridge",
        output="screen",
        parameters=[LaunchConfiguration("bridge_params_file")],
    )

    mock_load_node = Node(
        package="tb3_vda5050_bridge",
        executable="mock_load_publisher.py",
        name="mock_load_publisher",
        output="screen",
        condition=IfCondition(LaunchConfiguration("use_mock_load")),
    )

    return LaunchDescription([
        params_file_arg,
        use_mock_load_arg,
        bridge_node,
        mock_load_node
    ])
