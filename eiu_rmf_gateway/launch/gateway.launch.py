"""Start the RMF gateway with its parameter file."""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def gateway(context):
    params = [LaunchConfiguration('params_file')]
    nav_graph_path = LaunchConfiguration('nav_graph_path').perform(context)
    if nav_graph_path:
        params.append({'nav_graph_path': nav_graph_path})
    return [Node(package='eiu_rmf_gateway', executable='gateway', name='eiu_rmf_gateway',
                 output='both', parameters=params)]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            'params_file',
            default_value=PathJoinSubstitution([FindPackageShare('eiu_rmf_gateway'), 'config', 'gateway.yaml']),
            description='Gateway parameters'),
        DeclareLaunchArgument(
            'nav_graph_path', default_value='',
            description='Nav graph file of the fleet adapter; empty = the value of params_file'),
        OpaqueFunction(function=gateway),
    ])
