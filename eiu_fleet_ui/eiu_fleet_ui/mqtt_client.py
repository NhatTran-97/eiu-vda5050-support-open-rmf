"""VDA5050 connectivity, over MQTT, for every robot the adapter is configured with.

Broker address and each robot's topic prefix come from the adapter's config, so
adding a robot there is all it takes for the UI to follow it — see config.py.

Only the `connection` topic is subscribed. Pose and battery already reach the
UI through /fleet_states in the RMF frame (see ros_bridge.py), which is the
frame the map and robot table use; subscribing to the raw VDA5050 `state` and
`visualization` topics as well would only duplicate that data in a different
frame that nothing renders. The one thing RMF's fleet-wide watchdog cannot
tell you — whether *this* robot's VDA5050 client is connected to the broker,
right now — is what this class exists for.
"""

import json
import threading

import paho.mqtt.client as mqtt
from PySide6.QtCore import QObject, Signal, Property, Slot, QTimer

from .config import FleetConfig, client_id

LEAF_CONNECTION = "connection"


class MqttClient(QObject):
    """
    Connects to the broker and tracks, per configured robot, whether its
    VDA5050 client currently reports itself connected.

    QML receives:
        mqtt.connected        -> bool (broker connection status)
        mqtt.robotsOnlineJson -> JSON object {robot_name: bool}
    """

    stateChanged = Signal()
    onlineChanged = Signal()

    def __init__(self, config: FleetConfig, parent=None):
        super().__init__(parent)

        self._config = config
        self._connected = False

        # Paho callbacks run outside Qt's UI thread; they only ever touch
        # _online under this lock. The Qt timer below is what publishes to
        # QML, so a burst of reconnects cannot flood Qt's event queue.
        self._lock = threading.Lock()
        self._online = {r.name: False for r in config.robots}
        self._dirty = False
        self._online_json = json.dumps(self._online)

        self._flush_timer = QTimer(self)
        self._flush_timer.setInterval(200)   # connection state changes rarely
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
            print(f"[MQTT] broker connected, subscribed to "
                  f"{len(self._config.robots)} connection topic(s)")
        else:
            print(f"[MQTT] connect failed rc={rc}")
        self.stateChanged.emit()

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        with self._lock:
            for name in self._online:
                self._online[name] = False
            self._dirty = True
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

        # { "connectionState": "ONLINE"/"OFFLINE"/"CONNECTIONBROKEN" }
        online = data.get("connectionState", "") == "ONLINE"
        with self._lock:
            if self._online.get(robot.name) != online:
                self._online[robot.name] = online
                self._dirty = True

    def _flush(self):
        """Move the latest online map onto the Qt thread and notify QML once."""
        with self._lock:
            if not self._dirty:
                return
            self._dirty = False
            payload = json.dumps(self._online)

        self._online_json = payload
        self.onlineChanged.emit()

    # ── QML Properties ────────────────────────────────────────────────────────

    @Property(bool, notify=stateChanged)
    def connected(self):
        return self._connected

    @Property(str, notify=onlineChanged)
    def robotsOnlineJson(self):
        return self._online_json
