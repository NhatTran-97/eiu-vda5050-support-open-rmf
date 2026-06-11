from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    pkg_share = FindPackageShare('vda5050_fleet_adapter')

    nav_graph_path = PathJoinSubstitution([pkg_share, 'maps', 'nav_graph.yaml'])

    return LaunchDescription([
        DeclareLaunchArgument('fleet_name',      default_value='tb3_fleet'),
        DeclareLaunchArgument('rmf_server_uri',  default_value=''),
        DeclareLaunchArgument('mqtt_broker_url', default_value='tcp://localhost:1883'),

        Node(
            package='vda5050_fleet_adapter',
            executable='vda5050_fleet_adapter',
            name='vda5050_fleet_adapter',
            output='screen',
            parameters=[
                PathJoinSubstitution([pkg_share, 'config', 'params.yaml']),
                {
                    'fleet_name':      LaunchConfiguration('fleet_name'),
                    'rmf_server_uri':  LaunchConfiguration('rmf_server_uri'),
                    'mqtt_broker_url': LaunchConfiguration('mqtt_broker_url'),
                    'nav_graph_path':  nav_graph_path,
                }
            ],
        ),
    ])
