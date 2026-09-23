"""Track VDA5050 connectivity and telemetry for configured robots over MQTT."""

import collections
import json
import threading
import time
from dataclasses import dataclass

import paho.mqtt.client as mqtt
from PySide6.QtCore import QObject, Signal, Property, Slot

from . import ui_settings
from .config import FleetConfig, client_id
from .vda5050 import traffic
from .vda5050.graph import NavGraph, OffGraphTracker
from .vda5050.state import parse_state

LEAF_CONNECTION = "connection"
LEAF_STATE = "state"
# Published by the fleet adapter to the robot; the dashboard only listens in.
LEAF_ORDER = "order"
LEAF_INSTANT = "instantActions"
# Mark robot telemetry stale after this many seconds without a state message.
STATE_STALE_AFTER_SEC = ui_settings.get("vda5050.state_stale_after_s")
TRAFFIC_LOG_SIZE = ui_settings.get("vda5050.traffic_log_size")
_FINAL_ACTION_STATUSES = ("FINISHED", "FAILED")


@dataclass(frozen=True)
class MqttSnapshot:
    """What the dashboard reads from MQTT at one moment; the dicts are not changed afterwards."""

    online: dict           # robot -> connected and not stale
    telemetry: dict        # robot -> exported state
    traffic: list          # message log, newest first, without raw payloads
    traffic_version: int   # changes whenever the log does


class MqttClient(QObject):
    """Follow per-robot connection, state, order and instantActions messages."""

    stateChanged = Signal()

    def __init__(self, config: FleetConfig, parent=None):
        super().__init__(parent)

        self._config = config
        self._connected = False
        # Robots to follow; grows and shrinks as fleets register or drop robots.
        self._robots = list(config.robots)

        # Protect data shared by the MQTT and Qt threads.
        self._lock = threading.Lock()
        self._online = {r.name: False for r in config.robots}
        self._telemetry = {}
        self._last_state_rx: dict[str, float] = {}
        self._last_state_epoch: dict[str, float] = {}
        # Exported telemetry per robot and the robots whose export is out of date.
        self._exports: dict[str, dict] = {}
        self._dirty: set[str] = set()
        self._stale_reported: dict[str, bool] = {}

        # Message log, newest last; raw payloads are fetched on demand via rawFor().
        self._traffic = collections.deque(maxlen=TRAFFIC_LOG_SIZE)
        self._traffic_seq = 0
        self._traffic_version = 0
        self._traffic_rows: list[dict] = []
        self._traffic_rows_version = -1
        # robot -> logged instantActions whose actions have not finished yet.
        self._open_instant: dict[str, list[dict]] = {}
        self._state_sig: dict[str, tuple] = {}
        # robot -> {actionId: blockingType}, learned from order/instantActions messages.
        self._blocking: dict[str, dict] = {}
        self._off_graph = None
        self._off_graph_state: dict[str, tuple] = {}
        # robot -> trimmed latest order for the node-details view.
        self._orders: dict[str, dict] = {}

        # paho-mqtt 2 takes its version 2 callbacks; paho-mqtt 1 has only the older ones.
        cid = client_id()
        if hasattr(mqtt, "CallbackAPIVersion"):
            self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
            self._client.on_connect = (
                lambda client, userdata, flags, reason, properties=None: self._on_connect(client, userdata, flags, reason))
            self._client.on_disconnect = (
                lambda client, userdata, flags, reason, properties=None: self._on_disconnect(client, userdata, reason))
        else:
            self._client = mqtt.Client(client_id=cid)
            self._client.on_connect = self._on_connect
            self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

        if self._config.username:
            self._client.username_pw_set(self._config.username, self._config.password)

    # Connect to and disconnect from MQTT.

    @Slot()
    def connect_broker(self):
        """Start the broker connection and MQTT worker."""
        host, port = self._config.broker_host, self._config.broker_port
        try:
            self._client.connect_async(host, port, keepalive=60)
            self._client.loop_start()   # Start the MQTT worker
            print(f"[MQTT] connecting → {host}:{port} "
                  f"({len(self._robots)} robot(s) from config)")
        except Exception as e:
            print(f"[MQTT] connect error: {e}")

    @Slot()
    def disconnect_broker(self):
        try:
            self._client.disconnect()
            self._client.loop_stop()
        except Exception:
            pass

    # Handle MQTT messages on the worker thread.

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            with self._lock:
                robots = list(self._robots)
            for robot in robots:
                self._subscribe(robot)
            print(f"[MQTT] broker connected, subscribed to "
                  f"{len(robots)} robot(s)' connection/state/order/instantActions topics")
        else:
            print(f"[MQTT] connect failed rc={rc}")
        self.stateChanged.emit()

    def _subscribe(self, robot):
        for leaf in (LEAF_CONNECTION, LEAF_STATE, LEAF_ORDER, LEAF_INSTANT):
            self._client.subscribe(robot.topic(leaf))

    def _robot_for_topic(self, topic: str):
        with self._lock:
            return next((r for r in self._robots if topic.startswith(r.topic_prefix)), None)

    def add_robot(self, robot):
        """Follow a robot that a fleet registered while the dashboard runs."""
        with self._lock:
            if any(r.name == robot.name for r in self._robots):
                return
            self._robots.append(robot)
            self._online[robot.name] = False
        if self._connected:
            self._subscribe(robot)
        print(f"[MQTT] following {robot.name} ({robot.topic_prefix})")

    def remove_robot(self, name: str):
        """Stop following a robot and drop what was known about it."""
        with self._lock:
            robot = next((r for r in self._robots if r.name == name), None)
            if robot is None:
                return
            self._robots.remove(robot)
            for table in (self._online, self._telemetry, self._last_state_rx, self._last_state_epoch,
                          self._exports, self._stale_reported, self._state_sig, self._blocking,
                          self._off_graph_state, self._orders, self._open_instant):
                table.pop(name, None)
            self._dirty.discard(name)
        if self._connected:
            for leaf in (LEAF_CONNECTION, LEAF_STATE, LEAF_ORDER, LEAF_INSTANT):
                self._client.unsubscribe(robot.topic(leaf))
        print(f"[MQTT] no longer following {name}")

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        with self._lock:
            for name in self._online:
                self._online[name] = False
        print(f"[MQTT] disconnected rc={rc}")
        self.stateChanged.emit()

    def _on_message(self, client, userdata, msg):
        robot = self._robot_for_topic(msg.topic)
        if robot is None:
            return
        try:
            data = json.loads(msg.payload)
        except Exception:
            return

        try:
            if msg.topic == robot.topic(LEAF_CONNECTION):
                online = data.get("connectionState", "") == "ONLINE"
                with self._lock:
                    if self._online.get(robot.name) != online:
                        self._online[robot.name] = online
                        self._log(robot.name, "in", "connection",
                                  "ONLINE" if online else "OFFLINE", msg.payload)
            elif msg.topic == robot.topic(LEAF_STATE):
                state = parse_state(data)
                sig = traffic.state_signature(state)
                with self._lock:
                    self._telemetry[robot.name] = state
                    if self._off_graph is not None:
                        self._off_graph_state[robot.name] = self._off_graph.update(
                            robot.name, state.x, state.y, time.monotonic())
                    self._last_state_rx[robot.name] = time.monotonic()
                    self._last_state_epoch[robot.name] = time.time()
                    self._dirty.add(robot.name)
                    # Log only meaningful state changes.
                    self._latch_action_status(robot.name, state)
                    if self._state_sig.get(robot.name) != sig:
                        self._state_sig[robot.name] = sig
                        self._log(robot.name, "in", "state", traffic.state_summary(state), msg.payload)
            elif msg.topic == robot.topic(LEAF_ORDER):
                with self._lock:
                    self._blocking.setdefault(robot.name, {}).update(traffic.order_actions(data))
                    self._orders[robot.name] = traffic.order_detail(data)
                    self._dirty.add(robot.name)    # action_states carry blockingType
                    self._log(robot.name, "out", "order", traffic.order_summary(data), msg.payload)
            elif msg.topic == robot.topic(LEAF_INSTANT):
                with self._lock:
                    actions = traffic.instant_actions(data)
                    self._blocking.setdefault(robot.name, {}).update(actions)
                    self._dirty.add(robot.name)
                    entry = self._log(robot.name, "out", "instantAction", traffic.instant_summary(data),
                                      msg.payload, ids=list(actions))
                    open_entries = self._open_instant.setdefault(robot.name, [])
                    open_entries.append(entry)
                    del open_entries[:-TRAFFIC_LOG_SIZE]
        except Exception as e:
            print(f"[MQTT] malformed payload on {msg.topic}: {e}")

    # Read on the GUI thread.

    def snapshot(self, now: float | None = None) -> MqttSnapshot:
        """Connection, telemetry and message log as of now; telemetry goes stale after STATE_STALE_AFTER_SEC."""
        now = time.monotonic() if now is None else now
        with self._lock:
            for name in self._telemetry:
                stale = (now - self._last_state_rx.get(name, 0.0)) > STATE_STALE_AFTER_SEC
                if self._stale_reported.get(name) != stale:
                    self._stale_reported[name] = stale
                    self._dirty.add(name)
            for name in self._dirty:
                state = self._telemetry.get(name)
                if state is not None:
                    self._exports[name] = self._export(name, state, self._stale_reported.get(name, False))
            self._dirty.clear()
            # A robot reads offline once its telemetry goes stale, even if its connection
            # topic never got a final OFFLINE (dead process, no broker LWT to catch it).
            online = {name: v and not self._stale_reported.get(name, False) for name, v in self._online.items()}
            if self._traffic_rows_version != self._traffic_version:
                self._traffic_rows = [{k: v for k, v in e.items() if k != "raw"} for e in reversed(self._traffic)]
                self._traffic_rows_version = self._traffic_version
            return MqttSnapshot(online=online, telemetry=dict(self._exports),
                                traffic=self._traffic_rows, traffic_version=self._traffic_version)

    def set_graph(self, graph: NavGraph):
        """Enable off-graph detection against the given navigation graph."""
        self._off_graph = OffGraphTracker(graph, limit=ui_settings.get("vda5050.off_graph_limit_m"),
                                          hold=ui_settings.get("vda5050.off_graph_hold_s"))

    def _export(self, name, state, stale):
        """Telemetry dict for QML; action states gain the blockingType the order declared."""
        d = state.to_dict()
        blocking = self._blocking.get(name, {})
        d["action_states"] = [
            {**a, "blockingType": blocking.get(a.get("actionId"), "")} if isinstance(a, dict) else a
            for a in d["action_states"]]
        d["order_detail"] = self._orders.get(name)
        d["off_graph_m"], d["off_graph"] = self._off_graph_state.get(name, (None, False))
        d["stale"] = stale
        d["last_rx"] = self._last_state_epoch.get(name, 0)
        return d

    def _latch_action_status(self, robot: str, state):
        """Keep each instantAction's last reported status. Caller holds self._lock."""
        entries = self._open_instant.get(robot)
        if not entries:
            return
        reported = {a.get("actionId"): a.get("actionStatus")
                    for a in state.action_states if isinstance(a, dict)}
        if not reported:
            return
        for e in entries:
            for action_id in e["ids"]:
                status = reported.get(action_id)
                if status and status != e["status"]:
                    e["status"] = status
                    self._traffic_version += 1
        self._open_instant[robot] = [e for e in entries if e["status"] not in _FINAL_ACTION_STATUSES]

    def _log(self, robot: str, direction: str, kind: str, summary: str, payload, ids=None) -> dict:
        """Append to the traffic log. Caller holds self._lock; ids = actionIds an instantAction carries."""
        self._traffic_seq += 1
        raw = payload.decode("utf-8", "replace") if isinstance(payload, (bytes, bytearray)) else str(payload)
        entry = {"id": self._traffic_seq, "ts": time.time(), "robot": robot,
                 "dir": direction, "type": kind, "summary": summary, "ids": ids or [],
                 "status": "SENT" if kind == "instantAction" else "", "raw": raw}
        self._traffic.append(entry)
        self._traffic_version += 1
        return entry

    # MQTT state exposed to QML.

    @Property(bool, notify=stateChanged)
    def connected(self):
        return self._connected

    @Slot(int, result=str)
    def rawFor(self, msg_id):
        """Pretty-printed original payload of one logged message ('' if it has aged out)."""
        with self._lock:
            raw = next((e["raw"] for e in self._traffic if e["id"] == msg_id), "")
        try:
            return json.dumps(json.loads(raw), indent=2)
        except Exception:
            return raw
