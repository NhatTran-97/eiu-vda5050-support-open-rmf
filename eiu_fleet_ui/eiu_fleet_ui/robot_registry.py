"""Follow robot registration over ROS and offer it to QML.

Carries registration requests and results between QML and the fleet adapters, which own the rules.
"""

import json
import queue
import time
import uuid

from PySide6.QtCore import QObject, Property, QTimer, Signal, Slot

from . import ui_settings
from .registry_model import RegistryModel

# JSON strings on std_msgs/String, shared with the fleet adapter and scripts/register_robot.py.
REQUEST_TOPIC = "/robot_registration_requests"
RESULT_TOPIC = "/robot_registration_results"
REGISTRY_TOPIC = "/robot_registry"
DISCOVERY_TOPIC = "/robot_discovery"

# Seconds to wait for an adapter's verdict before reporting no answer.
REPLY_TIMEOUT_SEC = ui_settings.get("registration.reply_timeout_s")
_EXPIRY_CHECK_MS = 1000


class RobotRegistry(QObject):
    """Registry of robots per fleet, robots waiting to be registered, and registration requests."""

    changed = Signal()
    robotAdded = Signal(object)       # RobotIdentity of a robot a fleet registered
    robotRemoved = Signal(str)        # name of a robot a fleet stopped tracking
    newRobotsDetected = Signal(str)   # JSON list of the keys of robots that just appeared
    checkResult = Signal(str)         # JSON verdict of a dry run
    requestResult = Signal(str)       # JSON result of an add or remove
    _incoming = Signal(str, str)      # kind, JSON text: ROS thread to GUI thread

    def __init__(self, configured=(), parent=None):
        super().__init__(parent)
        self._model = RegistryModel(configured)
        self._fleets_json = "[]"
        self._pending_json = "[]"
        self._conflicts_json = "[]"
        self._pending: list = []
        self._conflicts: list = []

        self._publisher = None
        self._string_type = None
        self._wake = None
        self._outgoing = queue.SimpleQueue()
        # request_id -> (kind, action, deadline) of requests that have no answer yet.
        self._waiting: dict = {}

        self._incoming.connect(self._on_incoming)
        self._expiry = QTimer(self)
        self._expiry.setInterval(_EXPIRY_CHECK_MS)
        self._expiry.timeout.connect(self._expire_requests)
        self._expiry.start()

    # ROS entities, created once the node exists.

    def attach(self, node):
        from rclpy.qos import QoSDurabilityPolicy, QoSHistoryPolicy, QoSProfile, QoSReliabilityPolicy
        from std_msgs.msg import String

        # Registry and discovery are snapshots: a late subscriber needs only the last one.
        latched = QoSProfile(history=QoSHistoryPolicy.KEEP_LAST, depth=1,
                             reliability=QoSReliabilityPolicy.RELIABLE,
                             durability=QoSDurabilityPolicy.TRANSIENT_LOCAL)
        volatile = QoSProfile(history=QoSHistoryPolicy.KEEP_LAST, depth=20,
                              reliability=QoSReliabilityPolicy.RELIABLE,
                              durability=QoSDurabilityPolicy.VOLATILE)

        self._string_type = String
        self._publisher = node.create_publisher(String, REQUEST_TOPIC, volatile)
        node.create_subscription(String, REGISTRY_TOPIC, lambda m: self._incoming.emit("registry", m.data), latched)
        node.create_subscription(String, DISCOVERY_TOPIC, lambda m: self._incoming.emit("discovery", m.data), latched)
        node.create_subscription(String, RESULT_TOPIC, lambda m: self._incoming.emit("result", m.data), volatile)
        self._wake = node.create_guard_condition(self._drain)
        # Requests queued before the node existed are sent once the executor spins.
        self._wake.trigger()

    def _drain(self):
        """Publish queued requests from the ROS thread."""
        while True:
            try:
                request_id, text = self._outgoing.get_nowait()
            except queue.Empty:
                return
            if self._publisher is None or self._publisher.get_subscription_count() == 0:
                self._incoming.emit("result", json.dumps(self._failure(
                    request_id, "no_adapter", "No fleet adapter is listening for registration requests.")))
                continue
            try:
                self._publisher.publish(self._string_type(data=text))
            except Exception as e:
                print(f"[REG] publish failed request_id={request_id}: {e}")

    # Incoming messages, on the GUI thread.

    @Slot(str, str)
    def _on_incoming(self, kind: str, text: str):
        try:
            data = json.loads(text)
        except ValueError:
            return

        if kind == "registry":
            change = self._model.apply_registry(data)
            for identity in change.added:
                print(f"[REG] following {identity.name} ({identity.fleet_name})")
                self.robotAdded.emit(identity)
            for name in change.removed:
                print(f"[REG] no longer following {name}")
                self.robotRemoved.emit(name)
            self._publish()
        elif kind == "discovery":
            self._model.apply_discovery(data)
            self._publish()
        elif kind == "result" and isinstance(data, dict):
            waiting = self._waiting.pop(data.get("request_id"), None)
            if waiting is None:
                return   # someone else's request
            (self.checkResult if waiting[0] == "check" else self.requestResult).emit(text)

    def _publish(self):
        fleets_json = json.dumps(self._model.fleets())
        self._pending = self._model.pending()
        pending_json = json.dumps(self._pending)
        self._conflicts = self._model.conflicts()
        conflicts_json = json.dumps(self._conflicts)
        changed = (fleets_json, pending_json, conflicts_json) != (self._fleets_json, self._pending_json,
                                                                  self._conflicts_json)
        if conflicts_json != self._conflicts_json:
            for c in self._conflicts:
                print(f"[REG] robot name '{c['name']}' is used in fleets '{c['followed_fleet']}' and "
                      f"'{c['fleet']}' -- following the one in '{c['followed_fleet']}' only")
        self._fleets_json, self._pending_json, self._conflicts_json = fleets_json, pending_json, conflicts_json
        if changed:
            self.changed.emit()
        fresh = self._model.new_pending_keys()
        if fresh:
            self.newRobotsDetected.emit(json.dumps(fresh))

    # Requests from QML.

    def _send(self, kind: str, action: str, request: dict) -> str:
        request_id = "ui-" + uuid.uuid4().hex[:8]
        request.update({"action": action, "request_id": request_id})
        self._waiting[request_id] = (kind, action, time.monotonic() + REPLY_TIMEOUT_SEC)
        self._outgoing.put((request_id, json.dumps(request)))
        if self._wake is not None:
            self._wake.trigger()
        return request_id

    @staticmethod
    def _parse(request_json: str) -> dict:
        try:
            request = json.loads(request_json)
        except ValueError:
            request = None
        return request if isinstance(request, dict) else {}

    @Slot(str, result=str)
    def check(self, request_json: str) -> str:
        """Ask the fleet adapter to run its checks for adding a robot, without adding it."""
        request = self._parse(request_json)
        request["dry_run"] = True
        return self._send("check", "add", request)

    @Slot(str, result=str)
    def register(self, request_json: str) -> str:
        """Ask the fleet adapter to add a robot to a fleet."""
        request = self._parse(request_json)
        request["dry_run"] = False
        return self._send("request", "add", request)

    @Slot(str, str, result=str)
    def remove(self, fleet: str, name: str) -> str:
        """Ask the fleet adapter to decommission a robot and stop tracking it."""
        return self._send("request", "remove", {"fleet": fleet, "name": name})

    @Slot(str)
    def dismiss(self, key: str):
        """Hide a robot from the registration offers until the dashboard restarts."""
        self._model.dismiss(key)
        self._publish()

    @Slot(str, str, str, result=str)
    def suggestFor(self, fleet: str, manufacturer: str, serial: str) -> str:
        """Prefill values for a form: a free name and the first free charger of the fleet."""
        return json.dumps({"name": self._model.suggest_name(fleet, manufacturer, serial),
                           "charger": self._model.suggest_charger(fleet, manufacturer, serial)})

    @Slot(str, result=str)
    def sourceOf(self, name: str) -> str:
        """'config' or 'runtime': where a fleet's registry says the robot comes from."""
        return self._model.source_of(name)

    # Answers that never came.

    @staticmethod
    def _failure(request_id: str, code: str, message: str, action: str = "add") -> dict:
        return {"request_id": request_id, "action": action, "name": "", "ok": False,
                "needs_confirmation": False, "errors": [{"code": code, "message": message}],
                "warnings": [], "persisted": False}

    def _expire_requests(self):
        now = time.monotonic()
        for request_id, (kind, action, deadline) in list(self._waiting.items()):
            if now < deadline:
                continue
            del self._waiting[request_id]
            text = json.dumps(self._failure(
                request_id, "no_reply",
                f"The fleet adapter did not answer within {REPLY_TIMEOUT_SEC:.0f} s. Is it running?", action))
            (self.checkResult if kind == "check" else self.requestResult).emit(text)

    # Read by the dashboard.

    def pending_list(self) -> list:
        """Robots waiting to be registered, as pendingJson lists them."""
        return self._pending

    def conflict_list(self) -> list:
        """Robot names two fleets both use, as conflictsJson lists them."""
        return self._conflicts

    # Lists exposed to QML.

    @Property(str, notify=changed)
    def fleetsJson(self):
        return self._fleets_json

    @Property(str, notify=changed)
    def pendingJson(self):
        return self._pending_json

    @Property(str, notify=changed)
    def conflictsJson(self):
        return self._conflicts_json
