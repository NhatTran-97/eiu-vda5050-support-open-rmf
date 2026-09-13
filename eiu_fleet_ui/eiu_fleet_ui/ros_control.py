"""Direct per-robot control: pause/resume, speed limit, re-localize.

Talks to the services/parameter/topic OperatorInterface exposes (see
core/operator_interface.hpp in vda5050_fleet_adapter_full_control). Shares
RosBridge's rclpy node/executor, so attach() must run before it starts spinning.

QML slots only enqueue; the ROS executor thread drains and makes the actual
calls -- same thread-safety pattern as RosBridge's dispatch()/cancel_task().
"""

import json
import math
import queue
import threading

from PySide6.QtCore import QObject, Signal, Slot, Property

from .config import FleetConfig

ADAPTER_NODE = "vda5050_fleet_adapter_full_control"
SPEED_LIMIT_PREFIX = "speed_limit."
NO_SPEED_LIMIT = 0.0


class RosControl(QObject):
    """
    QML receives:
        control.speedLimitsJson -> JSON {robot_name: float}
        control.commandResult(robot, action, ok, message) -> Signal
    """

    speedLimitsChanged = Signal()
    commandResult = Signal(str, str, bool, str)

    def __init__(self, config: FleetConfig, parent=None):
        super().__init__(parent)
        self._config = config
        self._node = None

        self._pause_clients = {}
        self._resume_clients = {}
        self._init_pos_pubs = {}
        self._init_pos_result_subs = {}
        self._param_clients = {}

        self._lock = threading.Lock()
        self._speed_limits = {r.name: NO_SPEED_LIMIT for r in config.robots}
        self._speed_limits_json = json.dumps(self._speed_limits)

        self._command_queue = queue.SimpleQueue()

    # ── Setup: called from RosBridge.start(), before the executor spins ──────

    def attach(self, node):
        from std_srvs.srv import Trigger
        from std_msgs.msg import String
        from geometry_msgs.msg import PoseWithCovarianceStamped
        from rclpy.parameter_client import AsyncParameterClient

        self._node = node
        for robot in self._config.robots:
            prefix = f"/{ADAPTER_NODE}/{robot.name}"
            self._pause_clients[robot.name] = node.create_client(Trigger, f"{prefix}/pause")
            self._resume_clients[robot.name] = node.create_client(Trigger, f"{prefix}/resume")
            self._init_pos_pubs[robot.name] = node.create_publisher(
                PoseWithCovarianceStamped, f"{prefix}/init_position", 1)
            # The publish above is one-way; this carries the actual outcome.
            self._init_pos_result_subs[robot.name] = node.create_subscription(
                String, f"{prefix}/init_position_result",
                lambda msg, name=robot.name: self._on_init_position_result(name, msg), 1)
            self._param_clients[robot.name] = AsyncParameterClient(node, ADAPTER_NODE)

        node.create_timer(0.05, self._drain_commands)
        self._refresh_speed_limits()

    def _refresh_speed_limits(self):
        """Pull each robot's operator speed cap as currently set on the adapter."""
        from rcl_interfaces.msg import ParameterType

        for robot in self._config.robots:
            name = robot.name

            def _done(future, name=name):
                try:
                    values = future.result().values
                except Exception:
                    return
                if values and values[0].type == ParameterType.PARAMETER_DOUBLE:
                    with self._lock:
                        self._speed_limits[name] = values[0].double_value
                    self._publish_speed_limits()

            self._param_clients[name].get_parameters([SPEED_LIMIT_PREFIX + name], callback=_done)

    def _publish_speed_limits(self):
        with self._lock:
            self._speed_limits_json = json.dumps(self._speed_limits)
        self.speedLimitsChanged.emit()

    # ── QML-facing slots ──────────────────────────────────────────────────────

    @Slot(str)
    def pauseRobot(self, name: str):
        self._command_queue.put(("pause", name, None))

    @Slot(str)
    def resumeRobot(self, name: str):
        self._command_queue.put(("resume", name, None))

    @Slot(str, float)
    def setSpeedLimit(self, name: str, mps: float):
        self._command_queue.put(("speed", name, float(mps)))

    @Slot(str, float, float, float)
    def initPosition(self, name: str, x: float, y: float, yaw: float):
        self._command_queue.put(("init_position", name, (x, y, yaw)))

    # ── ROS thread: drains the queue and issues the calls ─────────────────────

    def _drain_commands(self):
        while True:
            try:
                kind, name, payload = self._command_queue.get_nowait()
            except queue.Empty:
                return
            if kind == "pause":
                self._call_trigger(self._pause_clients, name, "pause")
            elif kind == "resume":
                self._call_trigger(self._resume_clients, name, "resume")
            elif kind == "speed":
                self._call_set_speed(name, payload)
            elif kind == "init_position":
                self._call_init_position(name, payload)

    def _call_trigger(self, clients: dict, name: str, action: str):
        from std_srvs.srv import Trigger

        client = clients.get(name)
        if client is None:
            self.commandResult.emit(name, action, False, "unknown robot")
            return
        if not client.service_is_ready():
            self.commandResult.emit(name, action, False, f"{action} service not available")
            return

        def _done(future, name=name, action=action):
            try:
                resp = future.result()
            except Exception as e:
                self.commandResult.emit(name, action, False, str(e))
                return
            self.commandResult.emit(name, action, resp.success, resp.message)

        client.call_async(Trigger.Request()).add_done_callback(_done)

    def _call_set_speed(self, name: str, mps: float):
        from rclpy.parameter import Parameter

        client = self._param_clients.get(name)
        if client is None:
            self.commandResult.emit(name, "speed_limit", False, "unknown robot")
            return

        def _done(future, name=name, mps=mps):
            try:
                results = future.result().results
            except Exception as e:
                self.commandResult.emit(name, "speed_limit", False, str(e))
                return
            ok = bool(results) and results[0].successful
            message = (results[0].reason if results else "") or "applied"
            if ok:
                with self._lock:
                    self._speed_limits[name] = mps
                self._publish_speed_limits()
            self.commandResult.emit(name, "speed_limit", ok, message)

        param = Parameter(SPEED_LIMIT_PREFIX + name, Parameter.Type.DOUBLE, mps)
        client.set_parameters([param], callback=_done)

    def _call_init_position(self, name: str, xyz_yaw):
        from geometry_msgs.msg import PoseWithCovarianceStamped

        pub = self._init_pos_pubs.get(name)
        if pub is None:
            self.commandResult.emit(name, "init_position", False, "unknown robot")
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
        # Result comes back asynchronously via _on_init_position_result.

    def _on_init_position_result(self, name: str, msg) -> None:
        ok = msg.data == "ok"
        self.commandResult.emit(name, "init_position", ok, msg.data)

    # ── QML Properties ────────────────────────────────────────────────────────

    @Property(str, notify=speedLimitsChanged)
    def speedLimitsJson(self):
        return self._speed_limits_json
