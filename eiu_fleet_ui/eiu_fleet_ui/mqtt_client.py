import json
import paho.mqtt.client as mqtt
from PySide6.QtCore import QObject, Signal, Property, Slot

BROKER_TOPIC_STATE = "TB3/v2/ROBOTIS/0001/state"
BROKER_TOPIC_VIZ   = "TB3/v2/ROBOTIS/0001/visualization"
BROKER_TOPIC_CONN  = "TB3/v2/ROBOTIS/0001/connection"


class MqttClient(QObject):
    """
    Kết nối MQTT, nhận trạng thái robot, phát signal cho QML.

    QML nhận:
        mqtt.connected      → bool (broker có kết nối không)
        mqtt.robotOnline    → bool (robot có online không)
        mqtt.posX/Y/theta   → float (vị trí robot)
        mqtt.battery        → float (0–100)
        mqtt.driving        → bool
        mqtt.orderId        → str
    """

    stateChanged = Signal()
    vizChanged   = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)

        self._connected    = False
        self._robot_online = False
        self._pos_x        = 0.0
        self._pos_y        = 0.0
        self._theta        = 0.0
        self._battery      = 0.0
        self._driving      = False
        self._order_id     = ""

        # Tương thích paho-mqtt cả v1 lẫn v2
        try:
            self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                                       client_id="eiu_fleet_ui")
        except AttributeError:
            self._client = mqtt.Client(client_id="eiu_fleet_ui")

        self._client.on_connect    = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message    = self._on_message

    # ── Kết nối / ngắt ───────────────────────────────────────────────────────

    @Slot(str, int)
    def connect_broker(self, host: str, port: int):
        """Gọi từ main.py sau khi QML load xong."""
        try:
            self._client.connect_async(host, port, keepalive=60)
            self._client.loop_start()   # thread MQTT chạy nền
            print(f"[MQTT] connecting → {host}:{port}")
        except Exception as e:
            print(f"[MQTT] connect error: {e}")

    @Slot()
    def disconnect_broker(self):
        try:
            self._client.loop_stop()
            self._client.disconnect()
        except Exception:
            pass

    # ── Paho callbacks (chạy trong MQTT thread — Qt queue signal về main) ────

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            client.subscribe(BROKER_TOPIC_STATE)
            client.subscribe(BROKER_TOPIC_VIZ)
            client.subscribe(BROKER_TOPIC_CONN)
            print("[MQTT] broker connected, subscribed to TB3 topics")
        else:
            print(f"[MQTT] connect failed rc={rc}")
        self.stateChanged.emit()

    def _on_disconnect(self, client, userdata, rc):
        self._connected    = False
        self._robot_online = False
        print(f"[MQTT] disconnected rc={rc}")
        self.stateChanged.emit()

    def _on_message(self, client, userdata, msg):
        try:
            data  = json.loads(msg.payload)
            topic = msg.topic
        except Exception:
            return

        if topic == BROKER_TOPIC_VIZ:
            # VDA5050 viz: { "agvPosition": { "x": .., "y": .., "theta": .. } }
            pos = data.get("agvPosition", {})
            self._pos_x = float(pos.get("x",     self._pos_x))
            self._pos_y = float(pos.get("y",     self._pos_y))
            self._theta = float(pos.get("theta", self._theta))
            self.vizChanged.emit()

        elif topic == BROKER_TOPIC_CONN:
            # VDA5050 connection: { "connectionState": "ONLINE"/"OFFLINE"/"CONNECTIONBROKEN" }
            self._robot_online = data.get("connectionState", "") == "ONLINE"
            self.stateChanged.emit()

        elif topic == BROKER_TOPIC_STATE:
            # VDA5050 state: { "orderId":"..", "driving":bool, "batteryState":{"batteryCharge":..} }
            self._battery  = float(data.get("batteryState", {}).get("batteryCharge", self._battery))
            self._driving  = bool(data.get("driving",  self._driving))
            self._order_id = str(data.get("orderId",  self._order_id))
            self.stateChanged.emit()

    # ── QML Properties ────────────────────────────────────────────────────────

    @Property(bool, notify=stateChanged)
    def connected(self):    return self._connected

    @Property(bool, notify=stateChanged)
    def robotOnline(self):  return self._robot_online

    @Property(float, notify=vizChanged)
    def posX(self):         return self._pos_x

    @Property(float, notify=vizChanged)
    def posY(self):         return self._pos_y

    @Property(float, notify=vizChanged)
    def theta(self):        return self._theta

    @Property(float, notify=stateChanged)
    def battery(self):      return self._battery

    @Property(bool, notify=stateChanged)
    def driving(self):      return self._driving

    @Property(str, notify=stateChanged)
    def orderId(self):      return self._order_id
