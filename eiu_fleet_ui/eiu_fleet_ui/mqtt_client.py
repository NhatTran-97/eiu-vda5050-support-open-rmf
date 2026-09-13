"""VDA5050 connectivity + telemetry over MQTT, for every configured robot.

Broker address and each robot's topic prefix come from the adapter's config, so
adding a robot there is all it takes for the UI to follow it — see config.py.
Message parsing itself lives in vda5050/state.py (no Qt dependency); this
class only owns the paho connection, per-robot dispatch, and Qt-side throttling.
"""

import json
import threading
import time

import paho.mqtt.client as mqtt
from PySide6.QtCore import QObject, Signal, Property, Slot, QTimer

from .config import FleetConfig, client_id
from .vda5050.state import parse_state

LEAF_CONNECTION = "connection"
LEAF_STATE = "state"
# A robot can stay MQTT-connected while its own state-publishing loop hangs;
# past this age since the last `state` message, telemetry is flagged stale.
STATE_STALE_AFTER_SEC = 5.0


class MqttClient(QObject):
    """
    Connects to the broker and tracks, per configured robot, connectivity and
    the latest parsed `state` message.

    QML receives:
        mqtt.connected        -> bool (broker connection status)
        mqtt.robotsOnlineJson -> JSON object {robot_name: bool}
        mqtt.telemetryJson    -> JSON object {robot_name: RobotState.to_dict() + "stale"}
    """

    stateChanged = Signal()
    onlineChanged = Signal()
    telemetryChanged = Signal()

    def __init__(self, config: FleetConfig, parent=None):
        super().__init__(parent)

        self._config = config
        self._connected = False

        # Paho callbacks run outside Qt's UI thread and only touch _online/_telemetry
        # under this lock; the Qt timer below flushes to QML, so bursts can't flood it.
        self._lock = threading.Lock()
        self._online = {r.name: False for r in config.robots}
        self._telemetry = {}
        self._last_state_rx: dict[str, float] = {}
        self._stale_reported: dict[str, bool] = {}
        self._online_dirty = False
        self._telemetry_dirty = False
        self._online_json = json.dumps(self._online)
        self._telemetry_json = "{}"

        self._flush_timer = QTimer(self)
        self._flush_timer.setInterval(200)
        self._flush_timer.timeout.connect(self._flush)
        self._flush_timer.start()

        # Compatible with both paho-mqtt v1 and v2
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

    # ── Connect / disconnect ─────────────────────────────────────────────────

    @Slot()
    def connect_broker(self):
        """Called from main.py once QML has finished loading."""
        host, port = self._config.broker_host, self._config.broker_port
        try:
            if not self._flush_timer.isActive():
                self._flush_timer.start()
            self._client.connect_async(host, port, keepalive=60)
            self._client.loop_start()   # MQTT background thread
            print(f"[MQTT] connecting → {host}:{port} "
                  f"({len(self._config.robots)} robot(s) from config)")
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

    # ── Paho callbacks (worker thread; updates are coalesced for Qt) ──────────

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            for robot in self._config.robots:
                client.subscribe(robot.topic(LEAF_CONNECTION))
                client.subscribe(robot.topic(LEAF_STATE))
            print(f"[MQTT] broker connected, subscribed to "
                  f"{len(self._config.robots)} robot(s)' connection+state topics")
        else:
            print(f"[MQTT] connect failed rc={rc}")
        self.stateChanged.emit()

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        with self._lock:
            for name in self._online:
                self._online[name] = False
            self._online_dirty = True
        print(f"[MQTT] disconnected rc={rc}")
        self.stateChanged.emit()

    def _on_message(self, client, userdata, msg):
        robot = self._config.robot_for_topic(msg.topic)
        if robot is None:
            return
        try:
            data = json.loads(msg.payload)
        except Exception:
            return

        # Paho re-raises an uncaught exception here (suppress_exceptions is off
        # by default), which would kill its background thread and freeze all
        # telemetry -- so a malformed payload (wrong type, wrong field type)
        # is logged and dropped instead of ever reaching that point.
        try:
            if msg.topic == robot.topic(LEAF_CONNECTION):
                online = data.get("connectionState", "") == "ONLINE"
                with self._lock:
                    if self._online.get(robot.name) != online:
                        self._online[robot.name] = online
                        self._online_dirty = True
            elif msg.topic == robot.topic(LEAF_STATE):
                state = parse_state(data)
                with self._lock:
                    self._telemetry[robot.name] = state
                    self._last_state_rx[robot.name] = time.monotonic()
                    self._telemetry_dirty = True
        except Exception as e:
            print(f"[MQTT] malformed payload on {msg.topic}: {e}")

    def _flush(self):
        """Move the latest maps onto the Qt thread and notify QML, once each."""
        now = time.monotonic()
        with self._lock:
            online_payload = json.dumps(self._online) if self._online_dirty else None
            self._online_dirty = False

            # Re-checked every tick, not just on a new message -- a robot that
            # simply stops publishing should still flip to stale eventually.
            stale_now = {name: (now - self._last_state_rx.get(name, 0.0)) > STATE_STALE_AFTER_SEC
                         for name in self._telemetry}
            if stale_now != self._stale_reported:
                self._stale_reported = stale_now
                self._telemetry_dirty = True

            telemetry_payload = None
            if self._telemetry_dirty:
                telemetry_payload = json.dumps({
                    name: {**s.to_dict(), "stale": stale_now.get(name, False)}
                    for name, s in self._telemetry.items()})
            self._telemetry_dirty = False

        if online_payload is not None:
            self._online_json = online_payload
            self.onlineChanged.emit()
        if telemetry_payload is not None:
            self._telemetry_json = telemetry_payload
            self.telemetryChanged.emit()

    # ── QML Properties ────────────────────────────────────────────────────────

    @Property(bool, notify=stateChanged)
    def connected(self):
        return self._connected

    @Property(str, notify=onlineChanged)
    def robotsOnlineJson(self):
        return self._online_json

    @Property(str, notify=telemetryChanged)
    def telemetryJson(self):
        return self._telemetry_json
