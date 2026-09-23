"""Provide per-robot pause, speed, and localization controls through ROS."""

import math
import queue
import threading
import time

from PySide6.QtCore import QObject, Qt, QTimer, Signal, Slot, Property

from . import ui_settings
from .config import FleetConfig

SPEED_LIMIT_PREFIX = "speed_limit."
NO_SPEED_LIMIT = 0.0
# A command the fleet adapter has not answered after this long is reported as failed.
COMMAND_TIMEOUT_SEC = ui_settings.get("commands.timeout_s")
_EXPIRY_CHECK_MS = 500


class RosControl(QObject):
    """Robot control commands, whether each is still waiting for the adapter, and their results."""

    pendingChanged = Signal()
    # robot, action, ok, message; every command gets exactly one, also when nothing answers.
    commandResult = Signal(str, str, bool, str)
    _result = Signal(str, str, bool, str)   # ROS thread to GUI thread

    def __init__(self, config: FleetConfig, parent=None):
        super().__init__(parent)
        # Robots to control; grows and shrinks as fleets register or drop robots.
        self._robots = list(config.robots)
        self._node = None
        self._wake = None

        self._pause_clients = {}
        self._resume_clients = {}
        self._init_pos_pubs = {}
        self._init_pos_result_subs = {}
        # One parameter client per fleet adapter node, shared by that adapter's robots.
        self._param_clients = {}

        self._lock = threading.Lock()
        self._speed_limits = {r.name: NO_SPEED_LIMIT for r in self._robots}

        self._command_queue = queue.SimpleQueue()
        # "robot/action" -> deadline of commands sent and not answered yet; GUI thread only.
        self._pending: dict[str, float] = {}

        self._result.connect(self._on_result, Qt.ConnectionType.QueuedConnection)
        self._expiry = QTimer(self)
        self._expiry.setInterval(_EXPIRY_CHECK_MS)
        self._expiry.timeout.connect(self._expire)
        self._expiry.start()

    # Set up robot control before the ROS executor starts.

    def attach(self, node):
        self._node = node
        for robot in list(self._robots):
            self._create_endpoints(robot)
        self._wake = node.create_guard_condition(self._drain_commands)
        self._refresh_speed_limits()
        # Commands queued before the node existed are sent once the executor spins.
        self._wake.trigger()

    def _adapter_of(self, name: str):
        with self._lock:
            return next((r.adapter_node for r in self._robots if r.name == name), None)

    def _create_endpoints(self, robot):
        """Create the services, topics and parameter client of one robot on the adapter's node."""
        from std_srvs.srv import Trigger
        from std_msgs.msg import String
        from geometry_msgs.msg import PoseWithCovarianceStamped
        from rclpy.parameter_client import AsyncParameterClient

        node = self._node
        prefix = f"/{robot.adapter_node}/{robot.name}"
        self._pause_clients[robot.name] = node.create_client(Trigger, f"{prefix}/pause")
        self._resume_clients[robot.name] = node.create_client(Trigger, f"{prefix}/resume")
        self._init_pos_pubs[robot.name] = node.create_publisher(
            PoseWithCovarianceStamped, f"{prefix}/init_position", 1)
        # Receive the adapter's position initialization result.
        self._init_pos_result_subs[robot.name] = node.create_subscription(
            String, f"{prefix}/init_position_result",
            lambda msg, name=robot.name: self._on_init_position_result(name, msg), 1)
        if robot.adapter_node not in self._param_clients:
            self._param_clients[robot.adapter_node] = AsyncParameterClient(node, robot.adapter_node)

    def _destroy_endpoints(self, name: str):
        node = self._node
        for clients in (self._pause_clients, self._resume_clients):
            client = clients.pop(name, None)
            if client is not None:
                node.destroy_client(client)
        publisher = self._init_pos_pubs.pop(name, None)
        if publisher is not None:
            node.destroy_publisher(publisher)
        subscription = self._init_pos_result_subs.pop(name, None)
        if subscription is not None:
            node.destroy_subscription(subscription)

    def add_robot(self, robot):
        """Control a robot that a fleet registered while the dashboard runs."""
        with self._lock:
            if any(r.name == robot.name for r in self._robots):
                return
            self._robots.append(robot)
            self._speed_limits[robot.name] = NO_SPEED_LIMIT
        self._enqueue(("add_robot", robot.name, robot))

    def remove_robot(self, name: str):
        """Stop controlling a robot."""
        with self._lock:
            self._robots = [r for r in self._robots if r.name != name]
            self._speed_limits.pop(name, None)
        self._enqueue(("remove_robot", name, None))

    def _refresh_speed_limits(self, only=None):
        """Pull the operator speed caps as currently set on the adapters, one request per adapter."""
        from rcl_interfaces.msg import ParameterType

        with self._lock:
            by_adapter = {}
            for r in self._robots:
                if only is None or r.name == only:
                    by_adapter.setdefault(r.adapter_node, []).append(r.name)
        for adapter, names in by_adapter.items():
            client = self._param_clients.get(adapter)
            if client is None:
                continue

            def _done(future, names=names):
                try:
                    values = future.result().values
                except Exception:
                    return
                with self._lock:
                    for name, value in zip(names, values):
                        if name in self._speed_limits and value.type == ParameterType.PARAMETER_DOUBLE:
                            self._speed_limits[name] = value.double_value

            client.get_parameters([SPEED_LIMIT_PREFIX + n for n in names], callback=_done)

    # Robot control actions exposed to QML.

    def _request(self, name: str, action: str, payload=None):
        """Queue a command unless the same one is still waiting for an answer."""
        key = f"{name}/{action}"
        if key in self._pending:
            self.commandResult.emit(name, action, False, f"A {action.replace('_', ' ')} is still waiting for the fleet adapter")
            return
        self._pending[key] = time.monotonic() + COMMAND_TIMEOUT_SEC
        self.pendingChanged.emit()
        self._enqueue((action, name, payload))

    def _enqueue(self, command):
        self._command_queue.put(command)
        if self._wake is not None:
            self._wake.trigger()

    @Slot(str)
    def pauseRobot(self, name: str):
        self._request(name, "pause")

    @Slot(str)
    def resumeRobot(self, name: str):
        self._request(name, "resume")

    @Slot(str, float)
    def setSpeedLimit(self, name: str, mps: float):
        self._request(name, "speed_limit", float(mps))

    @Slot(str, float, float, float)
    def initPosition(self, name: str, x: float, y: float, yaw: float):
        self._request(name, "init_position", (x, y, yaw))

    # Results, delivered on the GUI thread.

    @Slot(str, str, bool, str)
    def _on_result(self, name: str, action: str, ok: bool, message: str):
        if self._pending.pop(f"{name}/{action}", None) is not None:
            self.pendingChanged.emit()
        self.commandResult.emit(name, action, ok, message)

    def _expire(self):
        now = time.monotonic()
        expired = [key for key, deadline in self._pending.items() if now > deadline]
        for key in expired:
            name, action = key.rsplit("/", 1)
            self._on_result(name, action, False, f"No answer from the fleet adapter within {COMMAND_TIMEOUT_SEC:.0f} s")

    # Execute queued control actions on the ROS thread.

    def _drain_commands(self):
        while True:
            try:
                kind, name, payload = self._command_queue.get_nowait()
            except queue.Empty:
                return
            if kind == "add_robot":
                self._create_endpoints(payload)
                self._refresh_speed_limits(only=name)
            elif kind == "remove_robot":
                self._destroy_endpoints(name)
            elif kind == "pause":
                self._call_trigger(self._pause_clients, name, "pause")
            elif kind == "resume":
                self._call_trigger(self._resume_clients, name, "resume")
            elif kind == "speed_limit":
                self._call_set_speed(name, payload)
            elif kind == "init_position":
                self._call_init_position(name, payload)

    def _call_trigger(self, clients: dict, name: str, action: str):
        from std_srvs.srv import Trigger

        client = clients.get(name)
        if client is None:
            self._result.emit(name, action, False, "unknown robot")
            return
        if not client.service_is_ready():
            self._result.emit(name, action, False, f"{action} service not available")
            return

        def _done(future, name=name, action=action):
            try:
                resp = future.result()
            except Exception as e:
                self._result.emit(name, action, False, str(e))
                return
            self._result.emit(name, action, resp.success, resp.message)

        client.call_async(Trigger.Request()).add_done_callback(_done)

    def _call_set_speed(self, name: str, mps: float):
        from rclpy.parameter import Parameter

        client = self._param_clients.get(self._adapter_of(name))
        if client is None:
            self._result.emit(name, "speed_limit", False, "unknown robot")
            return
        if not client.services_are_ready():
            self._result.emit(name, "speed_limit", False, "the fleet adapter's parameter service is not available")
            return

        def _done(future, name=name, mps=mps):
            try:
                results = future.result().results
            except Exception as e:
                self._result.emit(name, "speed_limit", False, str(e))
                return
            ok = bool(results) and results[0].successful
            message = (results[0].reason if results else "") or "applied"
            if ok:
                with self._lock:
                    self._speed_limits[name] = mps
            self._result.emit(name, "speed_limit", ok, message)

        param = Parameter(SPEED_LIMIT_PREFIX + name, Parameter.Type.DOUBLE, mps)
        client.set_parameters([param], callback=_done)

    def _call_init_position(self, name: str, xyz_yaw):
        from geometry_msgs.msg import PoseWithCovarianceStamped

        pub = self._init_pos_pubs.get(name)
        if pub is None:
            self._result.emit(name, "init_position", False, "unknown robot")
            return
        x, y, yaw = xyz_yaw
        msg = PoseWithCovarianceStamped()
        msg.header.frame_id = "map"
        msg.header.stamp = self._node.get_clock().now().to_msg()
        msg.pose.pose.position.x = x
        msg.pose.pose.position.y = y
        msg.pose.pose.orientation.z = math.sin(yaw / 2.0)
        msg.pose.pose.orientation.w = math.cos(yaw / 2.0)
        pub.publish(msg)
        # The adapter answers on <robot>/init_position_result.

    def _on_init_position_result(self, name: str, msg) -> None:
        self._result.emit(name, "init_position", msg.data == "ok", msg.data)

    def speed_limits(self) -> dict:
        """Applied operator speed cap by robot; zero means no cap."""
        with self._lock:
            return dict(self._speed_limits)

    # Control properties exposed to QML.

    @Property("QVariantMap", notify=pendingChanged)
    def pending(self):
        """'robot/action' -> true for every command still waiting for the fleet adapter."""
        return {key: True for key in self._pending}
