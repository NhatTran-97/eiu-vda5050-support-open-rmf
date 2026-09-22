"""Follow the fleet adapters' metrics reports over ROS and offer them to QML.

Subscribes to '/<adapter node>/metrics' of each fleet and hands the reports to metrics_model.MetricsModel.
"""

import json
import queue
import time

from PySide6.QtCore import QObject, Property, QTimer, Signal, Slot

from .config import env_float
from .metrics_model import DEFAULT_CAPACITY, DEFAULT_GRACE_S, DEFAULT_SILENT_FACTOR, MetricsModel

METRICS_TOPIC = "/{node}/metrics"

# Chart samples kept per adapter, missed reporting intervals before an adapter counts as silent, and the start-up grace
# (s) before an unseen adapter counts as missing; EIU_METRICS_HISTORY, EIU_METRICS_SILENT_FACTOR and EIU_ADAPTER_GRACE_S override them.
HISTORY_SAMPLES = int(env_float("EIU_METRICS_HISTORY", DEFAULT_CAPACITY))
SILENT_FACTOR = env_float("EIU_METRICS_SILENT_FACTOR", DEFAULT_SILENT_FACTOR)
GRACE_S = env_float("EIU_ADAPTER_GRACE_S", DEFAULT_GRACE_S)
_REFRESH_MS = 1000
_SUBSCRIBE_PERIOD_S = 0.5


class AdapterMetrics(QObject):
    """Health of the fleet adapters: their metrics for charts, and the problems worth an operator's attention."""

    changed = Signal()               # the snapshot changes every refresh
    attentionChanged = Signal()      # the attention items changed
    summaryChanged = Signal()        # how many adapters are found, or the level of that
    _incoming = Signal(str, str)      # adapter node, JSON text: ROS thread to GUI thread

    def __init__(self, robots=(), parent=None):
        super().__init__(parent)
        self._model = MetricsModel(HISTORY_SAMPLES, SILENT_FACTOR, started=time.monotonic(), grace_s=GRACE_S)
        self._node = None
        self._subscribed = set()
        # Adapter node -> whether its metrics topic has a publisher; the ROS thread writes, the GUI thread reads.
        self._present = {}
        self._summary = {"total": 0, "found": 0, "level": "wait"}
        self._to_subscribe = queue.SimpleQueue()
        self._metrics_json = json.dumps({"adapters": []})
        self._attention_json = "[]"
        for robot in robots:
            self._expect(robot.adapter_node, robot.fleet_name)

        self._incoming.connect(self._on_incoming)
        self._refresh_timer = QTimer(self)
        self._refresh_timer.setInterval(_REFRESH_MS)
        self._refresh_timer.timeout.connect(self._refresh)
        self._refresh_timer.start()
        self._refresh()

    def _expect(self, node_name, fleet):
        self._model.expect(node_name, fleet)
        if node_name not in self._subscribed:
            self._subscribed.add(node_name)
            self._to_subscribe.put(node_name)

    # ROS entities, created once the node exists.

    def attach(self, node):
        self._node = node
        node.create_timer(_SUBSCRIBE_PERIOD_S, self._on_timer)
        self._on_timer()

    def add_adapter(self, robot):
        """Follow the adapter of a robot that a fleet registered while the dashboard runs."""
        self._expect(robot.adapter_node, robot.fleet_name)

    def _on_timer(self):
        self._subscribe_pending()
        self._poll_presence()

    def _poll_presence(self):
        """Note which adapters publish their metrics topic right now; a node that is up shows a publisher at once."""
        if self._node is None:
            return
        for node_name in list(self._subscribed):
            try:
                self._present[node_name] = self._node.count_publishers(METRICS_TOPIC.format(node=node_name)) > 0
            except Exception:
                pass

    def _subscribe_pending(self):
        """Create the subscriptions of adapters not followed yet, on the ROS thread."""
        from std_msgs.msg import String

        while self._node is not None:
            try:
                node_name = self._to_subscribe.get_nowait()
            except queue.Empty:
                return
            self._node.create_subscription(
                String, METRICS_TOPIC.format(node=node_name),
                lambda msg, name=node_name: self._incoming.emit(name, msg.data), 10)

    # Reports, on the GUI thread.

    @Slot(str, str)
    def _on_incoming(self, node_name: str, text: str):
        try:
            report = json.loads(text)
        except ValueError:
            return
        self._model.apply(node_name, report, time.monotonic())
        self._refresh()

    def _refresh(self):
        now = time.monotonic()
        for node_name, present in dict(self._present).items():
            self._model.set_present(node_name, present)
        self._metrics_json = json.dumps(self._model.snapshot(now))
        self.changed.emit()
        summary = self._model.summary(now)
        if summary != self._summary:
            self._summary = summary
            self.summaryChanged.emit()
        attention = json.dumps(self._model.attention(now))
        if attention != self._attention_json:
            self._attention_json = attention
            self.attentionChanged.emit()

    # Read by QML.

    @Property(str, notify=changed)
    def metricsJson(self):
        return self._metrics_json

    @Property(int, notify=summaryChanged)
    def adaptersTotal(self):
        return self._summary["total"]

    @Property(int, notify=summaryChanged)
    def adaptersFound(self):
        return self._summary["found"]

    @Property(str, notify=summaryChanged)
    def adaptersLevel(self):
        """'wait' while unknown, 'ok' when every adapter is found, 'warn' when some are missing, 'err' when none is found."""
        return self._summary["level"]

    @Property(str, notify=attentionChanged)
    def attentionJson(self):
        return self._attention_json
