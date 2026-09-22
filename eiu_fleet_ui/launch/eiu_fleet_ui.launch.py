"""Launch the fleet UI with adapter and ROS domain settings."""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, SetEnvironmentVariable
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration, NotEqualsSubstitution, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    """Configure adapter paths and start the fleet UI node."""
    pkg = FindPackageShare("vda5050_fleet_adapter_full_control")
    config_dir = PathJoinSubstitution([pkg, "config"])

    # Default to this workspace's TB3 and AMR adapter configs.
    default_adapters = [
        config_dir, "/config_tb3.yaml=vda5050_fleet_adapter_tb3,",
        config_dir, "/config_amr.yaml=vda5050_fleet_adapter_amr",
    ]

    fleet_adapters_arg = DeclareLaunchArgument(
        "fleet_adapters",
        default_value=default_adapters,
        description="EIU_FLEET_ADAPTERS value: 'config_file=node_name,...', "
                    "one entry per fleet adapter the UI should control")
    ros_domain_id_arg = DeclareLaunchArgument(
        "ros_domain_id",
        default_value="",
        description="Optional EIU_ROS_DOMAIN_ID override; leave empty to use "
                    "this shell's ROS_DOMAIN_ID")

    nav_graph_arg = DeclareLaunchArgument(
        "nav_graph",
        default_value="",
        description="Optional EIU_NAV_GRAPH override: the nav graph file the map shows and the editor starts from; "
                    "leave empty to use the one beside the adapter config")

    return LaunchDescription([
        fleet_adapters_arg,
        ros_domain_id_arg,
        nav_graph_arg,
        SetEnvironmentVariable("EIU_FLEET_ADAPTERS", LaunchConfiguration("fleet_adapters")),
        SetEnvironmentVariable(
            "EIU_ROS_DOMAIN_ID", LaunchConfiguration("ros_domain_id"),
            condition=IfCondition(NotEqualsSubstitution(LaunchConfiguration("ros_domain_id"), ""))),
        SetEnvironmentVariable(
            "EIU_NAV_GRAPH", LaunchConfiguration("nav_graph"),
            condition=IfCondition(NotEqualsSubstitution(LaunchConfiguration("nav_graph"), ""))),
        Node(
            package="eiu_fleet_ui",
            executable="eiu_fleet_ui",
            output="both",
        ),
    ])
