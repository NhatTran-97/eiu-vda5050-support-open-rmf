"""The gateway's ROS 2 node.

Subscriptions record into the Recorder. Commands for Open-RMF are published at once; robot commands run on the
ROS thread and report their result later. The fleet adapters' nodes are found on the ROS graph: a robot belongs
to the node that offers `/<node>/<robot>/pause`, and `/<node>/metrics` is that node's metrics report.
"""

import json
import math
import queue
import re
import threading
import time

from geometry_msgs.msg import PoseWithCovarianceStamped
from rclpy.node import Node
from rclpy.parameter import Parameter
from rclpy.parameter_client import AsyncParameterClient
from rclpy.qos import QoSDurabilityPolicy, QoSHistoryPolicy, QoSProfile, QoSReliabilityPolicy
from rcl_interfaces.msg import ParameterType
from rmf_dispenser_msgs.msg import DispenserState
from rmf_fleet_msgs.msg import FleetState, LaneRequest, LaneStates
from rmf_ingestor_msgs.msg import IngestorState
from rmf_task_msgs.msg import ApiRequest, ApiResponse, DispatchStates
from std_msgs.msg import String
from std_srvs.srv import Trigger

from .core import Recorder, dispatch_states, fleet_robots, parse_json

RELIABLE_TL = QoSProfile(history=QoSHistoryPolicy.KEEP_LAST, depth=10, reliability=QoSReliabilityPolicy.RELIABLE,
                         durability=QoSDurabilityPolicy.TRANSIENT_LOCAL)
RELIABLE_VOLATILE = QoSProfile(history=QoSHistoryPolicy.KEEP_LAST, depth=1,
                               reliability=QoSReliabilityPolicy.RELIABLE, durability=QoSDurabilityPolicy.VOLATILE)
LATCHED = QoSProfile(history=QoSHistoryPolicy.KEEP_LAST, depth=1, reliability=QoSReliabilityPolicy.RELIABLE,
                     durability=QoSDurabilityPolicy.TRANSIENT_LOCAL)
VOLATILE_20 = QoSProfile(history=QoSHistoryPolicy.KEEP_LAST, depth=20, reliability=QoSReliabilityPolicy.RELIABLE,
                         durability=QoSDurabilityPolicy.VOLATILE)

PAUSE_SERVICE = re.compile(r'^/(?P<node>.+)/(?P<robot>[^/]+)/pause$')
KEEP = object()
METRICS_TOPIC = re.compile(r'^/(?P<node>[^/]+)/metrics$')
SPEED_LIMIT_PREFIX = 'speed_limit.'
CHECK_PERIOD_S = 0.5

PARAMETERS = {
    'redis_url': 'redis://127.0.0.1:6379/0',
    'redis_prefix': 'eiu:rmf',
    'flush_period_s': 0.1,
    'heartbeat_period_s': 1.0,
    'heartbeat_ttl_s': 3.0,
    'events_maxlen': 10000,
    'command_max_age_s': 30.0,
    'command_block_ms': 500,
    'task_events_host': '127.0.0.1',
    'task_events_port': 8100,
    'discovery_period_s': 2.0,
    'robot_command_timeout_s': 10.0,
    'nav_graph_path': '',
}


class RosIO(Node):
    def __init__(self, recorder: Recorder):
        super().__init__('eiu_rmf_gateway')
        for name, default in PARAMETERS.items():
            self.declare_parameter(name, default)
        self._recorder = recorder
        self._publish_lock = threading.Lock()
        self._queue: queue.SimpleQueue = queue.SimpleQueue()
        self._command_timeout = float(self.param('robot_command_timeout_s'))

        self._robot_node: dict[str, str] = {}
        self._known_robots: set[str] = set()
        self._triggers: dict[tuple[str, str], object] = {}
        self._init_pubs: dict[str, object] = {}
        self._param_clients: dict[str, AsyncParameterClient] = {}
        self._metrics_nodes: set[str] = set()
        # command id -> deadline; the ROS thread finishes each one exactly once.
        self._pending: dict[str, float] = {}
        self._pending_init: dict[str, str] = {}

        self._task_requests = self.create_publisher(ApiRequest, '/task_api_requests', RELIABLE_TL)
        self._lane_requests = self.create_publisher(LaneRequest, '/lane_closure_requests', RELIABLE_TL)
        self._registration_requests = self.create_publisher(String, '/robot_registration_requests', VOLATILE_20)

        self.create_subscription(FleetState, '/fleet_states', self._on_fleet_state, 10)
        self.create_subscription(ApiResponse, '/task_api_responses', self._on_response, RELIABLE_TL)
        self.create_subscription(DispatchStates, '/dispatch_states', self._on_dispatch_states, RELIABLE_VOLATILE)
        self.create_subscription(DispenserState, '/dispenser_states', lambda m: self._on_workcell(m, 'dispenser'), 10)
        self.create_subscription(IngestorState, '/ingestor_states', lambda m: self._on_workcell(m, 'ingestor'), 10)
        self.create_subscription(LaneStates, '/lane_states', self._on_lane_states, RELIABLE_TL)
        self.create_subscription(String, '/robot_registry', self._on_registry, LATCHED)
        self.create_subscription(String, '/robot_discovery', self._on_discovery, LATCHED)
        self.create_subscription(String, '/robot_registration_results', self._on_registration_result, VOLATILE_20)

        self._wake = self.create_guard_condition(self._drain)
        self.create_timer(float(self.param('discovery_period_s')), self._discover)
        self.create_timer(CHECK_PERIOD_S, self._expire)

    def param(self, name: str):
        return self.get_parameter(name).value

    # Commands, called from the gateway's command thread

    def execute(self, cmd_type: str, cmd_id: str, body: dict):
        if cmd_type == 'task_request':
            msg = ApiRequest(request_id=cmd_id, json_msg=json.dumps(body))
            with self._publish_lock:
                self._task_requests.publish(msg)
            return True, '', ''
        if cmd_type == 'lane_request':
            msg = LaneRequest(fleet_name=body['fleet'], open_lanes=list(body.get('open_lanes', [])),
                              close_lanes=list(body.get('close_lanes', [])))
            with self._publish_lock:
                self._lane_requests.publish(msg)
            return True, '', ''
        if cmd_type == 'registration_request':
            if self._registration_requests.get_subscription_count() == 0:
                return False, 'no_adapter', 'no fleet adapter listens for registration requests'
            request = dict(body['request'], request_id=cmd_id)
            with self._publish_lock:
                self._registration_requests.publish(String(data=json.dumps(request)))
            return True, '', ''
        if cmd_type in ('robot_pause', 'robot_resume', 'robot_speed_limit', 'robot_init_position'):
            if body['robot'] not in self._robot_node:
                return False, 'unknown_robot', f"no fleet adapter offers controls for {body['robot']}"
            self._queue.put((cmd_type, cmd_id, body))
            self._wake.trigger()
            return None
        return False, 'unknown_type', cmd_type

    # Robot commands, on the ROS thread

    def _drain(self):
        while True:
            try:
                cmd_type, cmd_id, body = self._queue.get_nowait()
            except queue.Empty:
                return
            robot = body['robot']
            self._pending[cmd_id] = time.monotonic() + self._command_timeout
            if cmd_type in ('robot_pause', 'robot_resume'):
                self._call_trigger(cmd_id, robot, 'pause' if cmd_type == 'robot_pause' else 'resume')
            elif cmd_type == 'robot_speed_limit':
                self._set_speed_limit(cmd_id, robot, float(body['mps']))
            else:
                self._init_position(cmd_id, robot, float(body['x']), float(body['y']), float(body['yaw']))

    def _finish(self, cmd_id: str, ok: bool, error: str = '', message: str = '') -> None:
        if self._pending.pop(cmd_id, None) is not None:
            self._recorder.command_result(cmd_id, ok, error, message)

    def _call_trigger(self, cmd_id: str, robot: str, action: str):
        client = self._triggers.get((robot, action))
        if client is None or not client.service_is_ready():
            self._finish(cmd_id, False, 'service_unavailable', f'{action} service of {robot} is not available')
            return

        def done(future):
            try:
                response = future.result()
            except Exception as e:
                self._finish(cmd_id, False, 'call_failed', str(e))
                return
            if response.success:
                self._record_control(robot, self._robot_node.get(robot, ''), None, paused=action == 'pause')
            self._finish(cmd_id, response.success, '' if response.success else 'refused', response.message)

        client.call_async(Trigger.Request()).add_done_callback(done)

    def _set_speed_limit(self, cmd_id: str, robot: str, mps: float):
        node = self._robot_node[robot]
        client = self._param_clients.get(node)
        if client is None or not client.services_are_ready():
            self._finish(cmd_id, False, 'service_unavailable', f'the parameter service of {node} is not available')
            return

        def done(future):
            try:
                results = future.result().results
            except Exception as e:
                self._finish(cmd_id, False, 'call_failed', str(e))
                return
            ok = bool(results) and results[0].successful
            if ok:
                self._record_control(robot, node, mps)
            self._finish(cmd_id, ok, '' if ok else 'refused', (results[0].reason if results else '') or '')

        client.set_parameters([Parameter(SPEED_LIMIT_PREFIX + robot, Parameter.Type.DOUBLE, mps)], callback=done)

    def _init_position(self, cmd_id: str, robot: str, x: float, y: float, yaw: float):
        pub = self._init_pubs.get(robot)
        if pub is None:
            self._finish(cmd_id, False, 'service_unavailable', f'init_position of {robot} is not available')
            return
        msg = PoseWithCovarianceStamped()
        msg.header.frame_id = 'map'
        msg.header.stamp = self.get_clock().now().to_msg()
        msg.pose.pose.position.x = x
        msg.pose.pose.position.y = y
        msg.pose.pose.orientation.z = math.sin(yaw / 2.0)
        msg.pose.pose.orientation.w = math.cos(yaw / 2.0)
        previous = self._pending_init.pop(robot, None)
        if previous:
            self._finish(previous, False, 'superseded', 'a newer position was sent')
        self._pending_init[robot] = cmd_id
        pub.publish(msg)

    def _on_init_result(self, robot: str, msg):
        cmd_id = self._pending_init.pop(robot, None)
        if cmd_id:
            ok = msg.data == 'ok'
            self._finish(cmd_id, ok, '' if ok else 'refused', msg.data)

    def _expire(self):
        now = time.monotonic()
        for cmd_id in [c for c, deadline in self._pending.items() if now > deadline]:
            self._pending_init = {r: c for r, c in self._pending_init.items() if c != cmd_id}
            self._finish(cmd_id, False, 'no_answer', f'no answer within {self._command_timeout:.0f} s')

    # Fleet adapter nodes found on the ROS graph

    def _discover(self):
        found: dict[str, str] = {}
        for name, types in self.get_service_names_and_types():
            m = PAUSE_SERVICE.match(name)
            if m and 'std_srvs/srv/Trigger' in types and m['robot'] in self._known_robots:
                found[m['robot']] = m['node']
        for robot, node in found.items():
            if self._robot_node.get(robot) != node:
                self._add_controls(robot, node)
        for robot in [r for r in self._robot_node if r not in found]:
            self._remove_controls(robot)

        for name, types in self.get_topic_names_and_types():
            m = METRICS_TOPIC.match(name)
            if m and 'std_msgs/msg/String' in types and m['node'] not in self._metrics_nodes:
                self._metrics_nodes.add(m['node'])
                self.create_subscription(String, name, lambda msg, node=m['node']: self._on_metrics(node, msg), 10)

        nodes = set(self._robot_node.values()) | self._metrics_nodes
        for node in nodes:
            robots = sorted(r for r, n in self._robot_node.items() if n == node)
            self._recorder.put('adapters', node, {'node': node, 'robots': robots,
                                                  'metrics_topic': node in self._metrics_nodes})

    def _add_controls(self, robot: str, node: str):
        self._remove_controls(robot)
        prefix = f'/{node}/{robot}'
        self._robot_node[robot] = node
        self._triggers[(robot, 'pause')] = self.create_client(Trigger, f'{prefix}/pause')
        self._triggers[(robot, 'resume')] = self.create_client(Trigger, f'{prefix}/resume')
        self._init_pubs[robot] = self.create_publisher(PoseWithCovarianceStamped, f'{prefix}/init_position', 1)
        self.create_subscription(String, f'{prefix}/init_position_result',
                                 lambda msg, r=robot: self._on_init_result(r, msg), 1)
        if node not in self._param_clients:
            self._param_clients[node] = AsyncParameterClient(self, node)
        self._record_control(robot, node, None, paused=None)
        self._read_speed_limit(robot, node)

    def _remove_controls(self, robot: str):
        node = self._robot_node.pop(robot, None)
        if node is None:
            return
        for action in ('pause', 'resume'):
            client = self._triggers.pop((robot, action), None)
            if client is not None:
                self.destroy_client(client)
        pub = self._init_pubs.pop(robot, None)
        if pub is not None:
            self.destroy_publisher(pub)
        self._recorder.put('controls', robot, {'robot': robot, 'node': '', 'speed_limit': None, 'paused': None,
                                               'available': False})

    def _read_speed_limit(self, robot: str, node: str):
        client = self._param_clients[node]

        def done(future):
            try:
                value = future.result().values[0]
            except Exception:
                return
            if value.type == ParameterType.PARAMETER_DOUBLE:
                self._record_control(robot, node, value.double_value)

        if client.services_are_ready():
            client.get_parameters([SPEED_LIMIT_PREFIX + robot], callback=done)

    def _record_control(self, robot: str, node: str, speed_limit, paused=KEEP):
        """Controls of a robot; `paused` is the result of the last pause or resume through this gateway
        (None = unknown), since the adapter does not report it on ROS 2."""
        previous = self._recorder.get('controls', robot) or {}
        if speed_limit is None:
            speed_limit = previous.get('speed_limit')
        if paused is KEEP:
            paused = previous.get('paused')
        self._recorder.put('controls', robot, {'robot': robot, 'node': node, 'speed_limit': speed_limit,
                                               'paused': paused, 'available': True})

    # Subscriptions

    def _on_fleet_state(self, msg):
        robots = fleet_robots(msg)
        self._known_robots.update(r['name'] for r in robots)
        self._recorder.set_fleet(msg.name, robots)

    def _on_response(self, msg):
        self._recorder.add_event('task_api_response', {'request_id': msg.request_id,
                                                       'response': parse_json(msg.json_msg)})

    def _on_dispatch_states(self, msg):
        self._recorder.add_event('dispatch_states', {'states': dispatch_states(msg)})

    def _on_workcell(self, msg, kind: str):
        self._recorder.set_workcell(msg.guid, kind, bool(msg.request_guid_queue), float(msg.seconds_remaining))

    def _on_lane_states(self, msg):
        self._recorder.put('lanes', msg.fleet_name, {'fleet': msg.fleet_name,
                                                     'closed_lanes': sorted(int(i) for i in msg.closed_lanes),
                                                     'received_ms': int(time.time() * 1000)})

    def _on_registry(self, msg):
        data = parse_json(msg.data)
        if isinstance(data, dict) and data.get('fleet'):
            self._recorder.put('registry', data['fleet'], {'fleet': data['fleet'], 'registry': data})

    def _on_discovery(self, msg):
        data = parse_json(msg.data)
        if isinstance(data, dict) and data.get('reporter'):
            self._recorder.put('discovery', data['reporter'], {'reporter': data['reporter'], 'snapshot': data})

    def _on_registration_result(self, msg):
        data = parse_json(msg.data)
        if isinstance(data, dict) and data.get('request_id'):
            self._recorder.add_event('registration_result', {'id': data['request_id'], 'result': data})

    def _on_metrics(self, node: str, msg):
        report = parse_json(msg.data)
        if isinstance(report, dict):
            self._recorder.put('metrics', node, {'node': node, 'received_ms': int(time.time() * 1000),
                                                 'report': report}, always=True)
