import json
import os

from ament_index_python.packages import get_package_share_directory

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription, AppendEnvironmentVariable, TimerAction, ExecuteProcess, OpaqueFunction
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from nav2_common.launch import ReplaceString


EXTRA_ROBOT_POSES = ((12.0, -7.5), (14.0, -7.5))


def spawn_extra_robots(context, sdf_path, robot_description, nav2_launch, count):
    """Give each extra Burger isolated Gazebo topics, TF and Nav2 nodes."""
    with open(sdf_path, 'r') as f:
        base_sdf = f.read()

    actions = []
    for index, (x, y) in enumerate(EXTRA_ROBOT_POSES[:int(count.perform(context)) - 1], start=2):
        name = f'burger_{index}'
        sdf = base_sdf.replace('model name="turtlebot3_burger"', f'model name="{name}"')
        for old, new in (
            ('<topic>imu</topic>', f'<topic>/{name}/imu</topic>'),
            ('<topic>scan</topic>', f'<topic>/{name}/scan</topic>'),
            ('<topic>cmd_vel</topic>', f'<topic>/{name}/cmd_vel</topic>'),
            ('<odom_topic>odom</odom_topic>', f'<odom_topic>/{name}/odom</odom_topic>'),
            ('<tf_topic>/tf</tf_topic>', f'<tf_topic>/{name}/tf</tf_topic>'),
            ('<topic>joint_states</topic>', f'<topic>/{name}/joint_states</topic>'),
        ):
            sdf = sdf.replace(old, new)

        bridge_topics = [
            f'/{name}/joint_states@sensor_msgs/msg/JointState[gz.msgs.Model',
            f'/{name}/odom@nav_msgs/msg/Odometry[gz.msgs.Odometry',
            f'/{name}/tf@tf2_msgs/msg/TFMessage[gz.msgs.Pose_V',
            f'/{name}/cmd_vel@geometry_msgs/msg/TwistStamped]gz.msgs.Twist',
            f'/{name}/imu@sensor_msgs/msg/Imu[gz.msgs.IMU',
            f'/{name}/scan@sensor_msgs/msg/LaserScan[gz.msgs.LaserScan',
        ]
        actions.append(Node(
            package='ros_gz_bridge', executable='parameter_bridge',
            name=f'{name}_bridge', arguments=bridge_topics, output='screen',
        ))
        actions.append(Node(
            package='robot_state_publisher', executable='robot_state_publisher',
            namespace=name, name='robot_state_publisher', output='screen',
            parameters=[{'use_sim_time': LaunchConfiguration('use_sim_time'),
                         'robot_description': robot_description}],
            remappings=[('/tf', 'tf'), ('/tf_static', 'tf_static')],
        ))
        actions.append(TimerAction(
            period=float(index * 2),
            actions=[Node(
                package='ros_gz_sim',
                executable='create',
                name=f'spawn_{name}',
                arguments=['-name', name, '-string', sdf,
                           '-x', str(x), '-y', str(y), '-z', '0.01'],
                output='screen',
            )],
        ))
        actions.append(TimerAction(
            period=float(10 + index * 2),
            actions=[IncludeLaunchDescription(
                PythonLaunchDescriptionSource(nav2_launch),
                launch_arguments={
                    'namespace': name,
                    'use_namespace': 'True',
                    'slam': LaunchConfiguration('slam'),
                    'map': LaunchConfiguration('map'),
                    'use_sim_time': LaunchConfiguration('use_sim_time'),
                    'params_file': ReplaceString(
                        source_file=LaunchConfiguration('params_file'),
                        replacements={'topic: /scan': f'topic: /{name}/scan'},
                    ),
                    'autostart': LaunchConfiguration('autostart'),
                    'use_composition': LaunchConfiguration('use_composition'),
                    'use_respawn': LaunchConfiguration('use_respawn'),
                }.items(),
            )],
        ))
        initial_pose = {
            'header': {'frame_id': 'map'},
            'pose': {'pose': {'position': {'x': x, 'y': y, 'z': 0.0},
                              'orientation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0}},
                     'covariance': [0.25, 0, 0, 0, 0, 0,
                                    0, 0.25, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0.0685]}
        }
        actions.append(TimerAction(
            period=float(26 + index * 2),
            actions=[ExecuteProcess(
                cmd=['ros2', 'topic', 'pub', '--once',
                     '--wait-matching-subscriptions', '1',
                     f'/{name}/initialpose',
                     'geometry_msgs/msg/PoseWithCovarianceStamped',
                     json.dumps(initial_pose)],
                output='screen',
            )],
        ))
        actions.append(ExecuteProcess(
            cmd=['ros2', 'topic', 'pub', '--rate', '1',
                 f'/{name}/battery_state', 'sensor_msgs/msg/BatteryState',
                 '{"voltage": 12.0, "percentage": 1.0, '
                 '"power_supply_status": 2, "present": true}'],
            output='screen',
        ))
    return actions


def generate_launch_description():
    bringup_dir = get_package_share_directory('nav2_bringup')
    launch_dir = os.path.join(bringup_dir, 'launch')
    tb3_simulation_dir = get_package_share_directory('tb3_simulation')
    ros_gz_sim_dir = get_package_share_directory('ros_gz_sim')
    tb3_gazebo_dir = get_package_share_directory('turtlebot3_gazebo')

    # Set TURTLEBOT3_MODEL
    os.environ['TURTLEBOT3_MODEL'] = 'burger'

    slam = LaunchConfiguration('slam')
    map_yaml_file = LaunchConfiguration('map')
    use_sim_time = LaunchConfiguration('use_sim_time')
    params_file = LaunchConfiguration('params_file')
    autostart = LaunchConfiguration('autostart')
    use_composition = LaunchConfiguration('use_composition')
    use_respawn = LaunchConfiguration('use_respawn')
    use_rviz = LaunchConfiguration('use_rviz')
    rviz_config_file = LaunchConfiguration('rviz_config_file')
    x_pose = LaunchConfiguration('x_pose', default='13.2')
    y_pose = LaunchConfiguration('y_pose', default='-11.5')
    robot_count = LaunchConfiguration('robot_count')

    declare_slam_cmd = DeclareLaunchArgument(
        'slam', default_value='False',
        description='Whether to run SLAM')

    declare_map_yaml_cmd = DeclareLaunchArgument(
        'map',
        default_value=os.path.join(
            tb3_simulation_dir, 'maps', 'tb3_world', 'map.yaml'),
        description='Full path to map yaml file')

    declare_use_sim_time_cmd = DeclareLaunchArgument(
        'use_sim_time', default_value='True',
        description='Use simulation clock if true')

    declare_params_file_cmd = DeclareLaunchArgument(
        'params_file',
        default_value=os.path.join(tb3_simulation_dir, 'config', 'nav2_params.yaml'),
        description='Full path to nav2 params file')

    declare_autostart_cmd = DeclareLaunchArgument(
        'autostart', default_value='True',
        description='Automatically startup the nav2 stack')

    declare_use_composition_cmd = DeclareLaunchArgument(
        'use_composition', default_value='False',
        description='Whether to use composed bringup')

    declare_use_respawn_cmd = DeclareLaunchArgument(
        'use_respawn', default_value='False',
        description='Whether to respawn if a node crashes')

    declare_use_rviz_cmd = DeclareLaunchArgument(
        'use_rviz', default_value='True',
        description='Whether to start RViz')

    declare_rviz_config_file_cmd = DeclareLaunchArgument(
        'rviz_config_file',
        default_value=os.path.join(
            bringup_dir, 'rviz', 'nav2_default_view.rviz'),
        description='Full path to RViz config file')

    declare_use_gz_gui_cmd = DeclareLaunchArgument(
        'use_gz_gui', default_value='False',
        description='Whether to start the Gazebo GUI client (tắt mặc định vì hay crash trong Docker)')

    declare_robot_count_cmd = DeclareLaunchArgument(
        'robot_count', default_value='3', choices=['1', '2', '3'],
        description='Number of TurtleBot3 robots and independent Nav2 stacks')

    # Append GZ_SIM_RESOURCE_PATH - dùng AppendEnvironmentVariable như mẫu
    set_gz_resource_path = AppendEnvironmentVariable(
        'GZ_SIM_RESOURCE_PATH',
        os.path.join(tb3_gazebo_dir, 'models')
    )

    set_gz_resource_path2 = AppendEnvironmentVariable(
        'GZ_SIM_RESOURCE_PATH',
        os.path.join(tb3_simulation_dir, 'maps', 'tb3_world', 'models')
    )
    set_gz_resource_path3 = AppendEnvironmentVariable(
    'GZ_SIM_RESOURCE_PATH',
    os.path.join(os.path.expanduser('~'), '.gz', 'fuel', 'fuel.gazebosim.org', '1.0', 'openrobotics', 'models')
    )   

    # URDF từ turtlebot3_gazebo - không có namespace issue
    urdf_path = os.path.join(
        tb3_gazebo_dir, 'urdf', 'turtlebot3_burger.urdf')
    with open(urdf_path, 'r') as f:
        robot_desc = f.read()

    # Robot state publisher
    robot_state_publisher_cmd = Node(
        package='robot_state_publisher',
        executable='robot_state_publisher',
        name='robot_state_publisher',
        output='screen',
        parameters=[{
            'use_sim_time': use_sim_time,
            'robot_description': robot_desc,
        }],
    )

    gzserver_cmd = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(ros_gz_sim_dir, 'launch', 'gz_sim.launch.py')),
        launch_arguments={
            'gz_args': ['-r -s -v2 ', os.path.join(
                tb3_simulation_dir, 'maps', 'tb3_world', 'tb3_world.world')],
            'on_exit_shutdown': 'True'
        }.items()
    )

    # Gazebo GUI client — chỉ chạy khi use_gz_gui:=True (mặc định tắt vì hay crash trong Docker)
    gzclient_cmd = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(ros_gz_sim_dir, 'launch', 'gz_sim.launch.py')),
        condition=IfCondition(LaunchConfiguration('use_gz_gui')),
        launch_arguments={
            'gz_args': '-g -v2 ',
            'on_exit_shutdown': 'False'
        }.items()
    )

    spawn_turtlebot_cmd = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(tb3_gazebo_dir, 'launch', 'spawn_turtlebot3.launch.py')),
        launch_arguments={
            'x_pose': x_pose,
            'y_pose': y_pose,
        }.items()
    )

    # Nav2 bringup
    bringup_cmd = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(launch_dir, 'bringup_launch.py')),
        launch_arguments={
            'namespace':       '',
            'use_namespace':   'False',
            'slam':            slam,
            'map':             map_yaml_file,
            'use_sim_time':    use_sim_time,
            'params_file':     params_file,
            'autostart':       autostart,
            'use_composition': use_composition,
            'use_respawn':     use_respawn,
        }.items()
    )


    # RViz
    rviz_cmd = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(launch_dir, 'rviz_launch.py')),
        condition=IfCondition(use_rviz),
        launch_arguments={
            'namespace':     '',
            'use_namespace': 'False',
            'rviz_config':   rviz_config_file,
            'use_sim_time':  use_sim_time,  
        }.items()
    )
    

    ld = LaunchDescription()

    ld.add_action(set_gz_resource_path)
    ld.add_action(set_gz_resource_path2)
    ld.add_action(set_gz_resource_path3)


    ld.add_action(declare_slam_cmd)
    ld.add_action(declare_map_yaml_cmd)
    ld.add_action(declare_use_sim_time_cmd)
    ld.add_action(declare_params_file_cmd)
    ld.add_action(declare_autostart_cmd)
    ld.add_action(declare_use_composition_cmd)
    ld.add_action(declare_use_respawn_cmd)
    ld.add_action(declare_use_rviz_cmd)
    ld.add_action(declare_rviz_config_file_cmd)
    ld.add_action(declare_use_gz_gui_cmd)
    ld.add_action(declare_robot_count_cmd)

    auto_initial_pose = ExecuteProcess(
        cmd=[
            'ros2', 'topic', 'pub', '--once', '--qos-reliability', 'reliable',
            '/initialpose',
            'geometry_msgs/msg/PoseWithCovarianceStamped',
            '{"header": {"frame_id": "map"}, "pose": {"pose": {"position": {"x": 13.2, "y": -11.5, "z": 0.0}, "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}}, "covariance": [0.25,0,0,0,0,0, 0,0.25,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0.0685]}}'
        ],
        output='screen'
    )

    # Simulation does not publish /battery_state — publish a constant 100% so the
    # VDA5050 bridge can forward a valid battery level to the fleet adapter.
    fake_battery_pub = ExecuteProcess(
        cmd=[
            'ros2', 'topic', 'pub', '--rate', '1',
            '/battery_state',
            'sensor_msgs/msg/BatteryState',
            '{"header": {"frame_id": ""}, "voltage": 12.0, "percentage": 1.0, "power_supply_status": 2, "present": true}'
        ],
        output='screen'
    )

    ld.add_action(gzserver_cmd)
    ld.add_action(gzclient_cmd)
    ld.add_action(robot_state_publisher_cmd)
    ld.add_action(spawn_turtlebot_cmd)
    ld.add_action(OpaqueFunction(
        function=spawn_extra_robots,
        args=[os.path.join(tb3_gazebo_dir, 'models', 'turtlebot3_burger', 'model.sdf'),
              robot_desc, os.path.join(launch_dir, 'bringup_launch.py'), robot_count],
    ))
    ld.add_action(fake_battery_pub)
    ld.add_action(TimerAction(period=10.0, actions=[bringup_cmd]))
    ld.add_action(TimerAction(period=12.0, actions=[rviz_cmd]))
    ld.add_action(TimerAction(period=20.0, actions=[auto_initial_pose]))

    return ld
