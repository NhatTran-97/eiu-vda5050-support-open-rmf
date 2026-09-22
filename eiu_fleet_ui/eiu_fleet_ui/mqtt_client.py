"""Track VDA5050 connectivity and telemetry for configured robots over MQTT."""

import collections
import json
import threading
import time

import paho.mqtt.client as mqtt
from PySide6.QtCore import QObject, Signal, Property, Slot, QTimer

from .config import FleetConfig, client_id, env_float
from .vda5050 import traffic
from .vda5050.graph import NavGraph, OffGraphTracker
from .vda5050.state import parse_state

LEAF_CONNECTION = "connection"
LEAF_STATE = "state"
# Published by the fleet adapter to the robot; the dashboard only listens in.
LEAF_ORDER = "order"
LEAF_INSTANT = "instantActions"
# Mark robot telemetry stale after this many seconds without a state message.
STATE_STALE_AFTER_SEC = env_float("EIU_STATE_STALE_AFTER", 5.0)
TRAFFIC_LOG_SIZE = 200


class MqttClient(QObject):
    """Publish per-robot connection and telemetry data to QML."""

    stateChanged = Signal()
    onlineChanged = Signal()
    telemetryChanged = Signal()
    trafficChanged = Signal()

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
        self._stale_reported: dict[str, bool] = {}
        self._online_dirty = False
        self._telemetry_dirty = False
        self._online_json = json.dumps(self._online)
        self._telemetry_json = "{}"

        # Message log, newest last; raw payloads are fetched on demand via rawFor().
        self._traffic = collections.deque(maxlen=TRAFFIC_LOG_SIZE)
        self._traffic_seq = 0
        self._traffic_dirty = False
        self._traffic_json = "[]"
        self._state_sig: dict[str, tuple] = {}
        # robot -> {actionId: blockingType}, learned from order/instantActions messages.
        self._blocking: dict[str, dict] = {}
        self._off_graph = None
        self._off_graph_state: dict[str, tuple] = {}
        # robot -> trimmed latest order for the node-details view.
        self._orders: dict[str, dict] = {}

        self._flush_timer = QTimer(self)
        self._flush_timer.setInterval(200)
        self._flush_timer.timeout.connect(self._flush)
        self._flush_timer.start()

        # Support both paho-mqtt callback versions.
        cid = client_id()
        try:
            self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
        except AttributeError:
            self._client = mqtt.Client(client_id=cid)

        if self._config.username:
            self._client.username_pw_set(self._config.username, self._config.password)

        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

    # Connect to and disconnect from MQTT.

    @Slot()
    def connect_broker(self):
        """Start the broker connection and MQTT worker."""
        host, port = self._config.broker_host, self._config.broker_port
        try:
            if not self._flush_timer.isActive():
                self._flush_timer.start()
            self._client.connect_async(host, port, keepalive=60)
            self._client.loop_start()   # Start the MQTT worker
            print(f"[MQTT] connecting → {host}:{port} "
                  f"({len(self._robots)} robot(s) from config)")
        except Exception as e:
            print(f"[MQTT] connect error: {e}")

    @Slot()
    def disconnect_broker(self):
        try:
            self._flush_timer.stop()
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
            self._online_dirty = True
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
                          self._stale_reported, self._state_sig, self._blocking, self._off_graph_state,
                          self._orders):
                table.pop(name, None)
            self._online_dirty = True
            self._telemetry_dirty = True
        if self._connected:
            for leaf in (LEAF_CONNECTION, LEAF_STATE, LEAF_ORDER, LEAF_INSTANT):
                self._client.unsubscribe(robot.topic(leaf))
        print(f"[MQTT] no longer following {name}")

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        with self._lock:
            for name in self._online:
                self._online[name] = False
            self._online_dirty = True
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
                        self._online_dirty = True
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
                    self._telemetry_dirty = True
                    # Log only meaningful state changes.
                    self._latch_action_status(robot.name, state)
                    if self._state_sig.get(robot.name) != sig:
                        self._state_sig[robot.name] = sig
                        self._log(robot.name, "in", "state", traffic.state_summary(state), msg.payload)
            elif msg.topic == robot.topic(LEAF_ORDER):
                with self._lock:
                    self._blocking.setdefault(robot.name, {}).update(traffic.order_actions(data))
                    self._orders[robot.name] = traffic.order_detail(data)
                    self._telemetry_dirty = True    # action_states carry blockingType
                    self._log(robot.name, "out", "order", traffic.order_summary(data), msg.payload)
            elif msg.topic == robot.topic(LEAF_INSTANT):
                with self._lock:
                    self._blocking.setdefault(robot.name, {}).update(traffic.instant_actions(data))
                    self._telemetry_dirty = True
                    self._log(robot.name, "out", "instantAction", traffic.instant_summary(data), msg.payload,
                              ids=list(traffic.instant_actions(data)))
        except Exception as e:
            print(f"[MQTT] malformed payload on {msg.topic}: {e}")

    def _flush(self):
        """Publish pending connection and telemetry changes to QML."""
        now = time.monotonic()
        # Copy what changed under the lock; serialising it happens after the MQTT thread is free again.
        with self._lock:
            online = dict(self._online) if self._online_dirty else None
            self._online_dirty = False

            # Check telemetry age on every refresh tick.
            stale_now = {name: (now - self._last_state_rx.get(name, 0.0)) > STATE_STALE_AFTER_SEC
                         for name in self._telemetry}
            if stale_now != self._stale_reported:
                self._stale_reported = stale_now
                self._telemetry_dirty = True

            telemetry = None
            if self._telemetry_dirty:
                telemetry = {name: self._export(name, s, stale_now.get(name, False))
                             for name, s in self._telemetry.items()}
            self._telemetry_dirty = False

            # Newest first; raw payloads are fetched separately via rawFor().
            traffic = ([{k: v for k, v in e.items() if k != "raw"} for e in reversed(self._traffic)]
                       if self._traffic_dirty else None)
            self._traffic_dirty = False

        if online is not None:
            self._online_json = json.dumps(online)
            self.onlineChanged.emit()
        if telemetry is not None:
            self._telemetry_json = json.dumps(telemetry)
            self.telemetryChanged.emit()
        if traffic is not None:
            self._traffic_json = json.dumps(traffic)
            self.trafficChanged.emit()

    def set_graph(self, graph: NavGraph):
        """Enable off-graph detection against the given navigation graph."""
        self._off_graph = OffGraphTracker(graph)

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
        reported = {a.get("actionId"): a.get("actionStatus")
                    for a in state.action_states if isinstance(a, dict)}
        if not reported:
            return
        for e in self._traffic:
            if e["type"] != "instantAction" or e["robot"] != robot:
                continue
            for action_id in e["ids"]:
                status = reported.get(action_id)
                if status and status != e["status"]:
                    e["status"] = status
                    self._traffic_dirty = True

    def _log(self, robot: str, direction: str, kind: str, summary: str, payload, ids=None):
        """Append to the traffic log. Caller holds self._lock; ids = actionIds an instantAction carries."""
        self._traffic_seq += 1
        raw = payload.decode("utf-8", "replace") if isinstance(payload, (bytes, bytearray)) else str(payload)
        self._traffic.append({"id": self._traffic_seq, "ts": time.time(), "robot": robot,
                              "dir": direction, "type": kind, "summary": summary, "ids": ids or [],
                              "status": "SENT" if kind == "instantAction" else "", "raw": raw})
        self._traffic_dirty = True

    # MQTT state exposed to QML.

    @Property(bool, notify=stateChanged)
    def connected(self):
        return self._connected

    @Property(str, notify=onlineChanged)
    def robotsOnlineJson(self):
        return self._online_json

    @Property(str, notify=trafficChanged)
    def trafficJson(self):
        return self._traffic_json

    @Slot(int, result=str)
    def rawFor(self, msg_id):
        """Pretty-printed original payload of one logged message ('' if it has aged out)."""
        with self._lock:
            raw = next((e["raw"] for e in self._traffic if e["id"] == msg_id), "")
        try:
            return json.dumps(json.loads(raw), indent=2)
        except Exception:
            return raw

    @Property(str, notify=telemetryChanged)
    def telemetryJson(self):
        return self._telemetry_json
