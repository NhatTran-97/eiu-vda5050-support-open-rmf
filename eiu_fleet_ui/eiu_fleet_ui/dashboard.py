"""Everything the dashboard's panels show, refreshed on the GUI thread at a fixed period.

The backends only record what arrives on their own threads. Each refresh takes one snapshot of
them, derives the views with dashboard_model, and hands QML keyed list models that change only
the rows that differ, plus values that notify only when they change.
"""

import time

from PySide6.QtCore import QObject, Property, QTimer, Signal, Slot

from . import dashboard_model as dm
from . import ui_settings
from .list_model import KeyedListModel


class UiConfig(QObject):
    """Operator thresholds and map limits from ui_settings.yaml, exposed to QML as `uiConfig`."""

    @Property(float, constant=True)
    def lowBatteryPercent(self):
        return float(ui_settings.get("operator.low_battery_percent"))

    @Property(float, constant=True)
    def mediumBatteryPercent(self):
        return max(float(ui_settings.get("operator.medium_battery_percent")), self.lowBatteryPercent)

    @Property(float, constant=True)
    def pickRadiusM(self):
        return float(ui_settings.get("map.pick_radius_m"))

    @Property(float, constant=True)
    def lanePickRadiusM(self):
        return float(ui_settings.get("map.lane_pick_radius_m"))

    @Property(float, constant=True)
    def minZoom(self):
        return float(ui_settings.get("map.min_zoom"))

    @Property(float, constant=True)
    def maxZoom(self):
        return float(ui_settings.get("map.max_zoom"))


class Dashboard(QObject):
    """View state of the fleet dashboard, exposed to QML as `dashboard`."""

    summaryChanged = Signal()
    robotRowsChanged = Signal()
    taskRowsChanged = Signal()
    telemetryChanged = Signal()
    onlineChanged = Signal()
    speedLimitsChanged = Signal()
    robotNamesChanged = Signal()
    onlineRobotNamesChanged = Signal()
    destinationsChanged = Signal()
    routesChanged = Signal()
    filtersChanged = Signal()
    refreshed = Signal()

    def __init__(self, settings, map_provider, ros, mqtt, control, registry, adapter_metrics,
                 period_s: float, limits: dm.Limits = dm.Limits(), parent=None):
        super().__init__(parent)
        self._settings = settings
        self._ros = ros
        self._mqtt = mqtt
        self._control = control
        self._registry = registry
        self._adapter_metrics = adapter_metrics
        self._limits = limits
        self._waypoints_by_name = {w["name"]: w for w in map_provider.waypoints() if w.get("name")}

        self._robots = KeyedListModel("key", self)
        self._robot_list = KeyedListModel("key", self)
        self._map_robots = KeyedListModel("key", self)
        self._attention = KeyedListModel("key", self)
        self._tasks = KeyedListModel("id", self)
        self._traffic = KeyedListModel("id", self)

        self._robot_filter = ""
        self._task_state_filter = "All"
        self._task_search = ""
        self._traffic_filter = "all"

        self._rows: list[dict] = []
        self._task_rows: list[dict] = []
        self._tasks_version = -1
        self._traffic_rows: list[dict] = []
        self._traffic_version = -1
        self._values = {"robotRows": [], "taskRows": [], "telemetry": {}, "online": {}, "speedLimits": {},
                        "robotNames": [], "onlineRobotNames": [], "destinations": {}, "routes": []}
        self._summary = self._summary_of([], [], [], "HEALTHY", "All services nominal", {})

        self._period_ms = max(20, int(period_s * 1000))
        self._timer = QTimer(self)
        self._timer.setInterval(self._period_ms)
        self._timer.timeout.connect(self.refresh)

    def start(self):
        self.refresh()
        self._timer.start()

    # Refresh.

    @Slot()
    def refresh(self):
        now = time.monotonic()
        rmf_rows = self._ros.robots_snapshot()
        tasks_version, tasks = self._ros.tasks_snapshot()
        mqtt = self._mqtt.snapshot(now)
        stale_fleets = self._ros.stale_fleets(now)

        rows = dm.display_robots(self._settings.known_robots(), rmf_rows, mqtt.telemetry, mqtt.online,
                                 tasks, self._settings.fleetName, {f["fleet"] for f in stale_fleets})
        self._rows = rows
        self._robots.set_rows(rows)
        self._robot_list.set_rows(dm.filter_robots(rows, self._robot_filter))
        self._map_robots.set_rows(dm.map_robots(rmf_rows, mqtt.online))

        if tasks_version != self._tasks_version:
            self._tasks_version = tasks_version
            self._task_rows = dm.task_rows(tasks)
            self._tasks.set_rows(dm.filter_tasks(self._task_rows, self._task_state_filter, self._task_search))
            self._set("taskRows", self._task_rows, self.taskRowsChanged)
            self._set("destinations", dm.active_destinations(tasks), self.destinationsChanged)

        if mqtt.traffic_version != self._traffic_version:
            self._traffic_version = mqtt.traffic_version
            self._traffic_rows = mqtt.traffic
            self._traffic.set_rows(dm.filter_traffic(self._traffic_rows, self._traffic_filter))

        items = dm.attention_items(
            rmf_online=self._ros.rmfOnline, mqtt_connected=self._mqtt.connected,
            active_conflicts=self._ros.activeConflicts, blocked_lanes=self._ros.blockedLanes,
            broker_clashes=self._settings.broker_conflicts(), adapter_items=self._adapter_metrics.attention_list(),
            robots=rows, telemetry=mqtt.telemetry, tasks=tasks, name_conflicts=self._registry.conflict_list(),
            pending=self._registry.pending_list(), stale_fleets=stale_fleets, limits=self._limits)
        self._attention.set_rows(items)
        level, detail = dm.health(items, self._ros.rmfOnline, self._mqtt.connected)

        destination_of = {r["name"]: r["task_destination"] for r in rows if r["task_destination"]}
        self._set("robotRows", rows, self.robotRowsChanged)
        self._set("telemetry", mqtt.telemetry, self.telemetryChanged)
        self._set("online", mqtt.online, self.onlineChanged)
        self._set("speedLimits", self._control.speed_limits(), self.speedLimitsChanged)
        self._set("robotNames", [r["name"] for r in rows], self.robotNamesChanged)
        self._set("onlineRobotNames", [r["name"] for r in rows if r["online"]], self.onlineRobotNamesChanged)
        self._set("routes", dm.routes(rmf_rows, mqtt.telemetry, mqtt.online, self._waypoints_by_name,
                                      destination_of), self.routesChanged)

        summary = self._summary_of(rows, tasks, items, level, detail, mqtt.online)
        if summary != self._summary:
            self._summary = summary
            self.summaryChanged.emit()
        self.refreshed.emit()

    def _set(self, name: str, value, signal):
        if self._values[name] != value:
            self._values[name] = value
            signal.emit()

    @staticmethod
    def _summary_of(rows, tasks, items, level, detail, online) -> dict:
        counts = dm.task_counts(tasks)
        return {
            "robotCount": len(rows),
            "onlineCount": sum(1 for v in online.values() if v),
            "fleetSummary": dm.fleet_status_summary(rows),
            "runningTasks": counts["underway"],
            "queuedTasks": counts["queued"],
            "completedTasks": counts["completed"],
            "taskTotal": len(tasks),
            "criticalCount": sum(1 for i in items if i["severity"] == "critical"),
            "warningCount": sum(1 for i in items if i["severity"] == "warning"),
            "healthLevel": level,
            "healthDetail": detail,
        }

    def _refilter(self):
        self._robot_list.set_rows(dm.filter_robots(self._rows, self._robot_filter))
        self._tasks.set_rows(dm.filter_tasks(self._task_rows, self._task_state_filter, self._task_search))
        self._traffic.set_rows(dm.filter_traffic(self._traffic_rows, self._traffic_filter))
        self.filtersChanged.emit()

    # Models.

    @Property(QObject, constant=True)
    def robots(self):
        """Every followed robot."""
        return self._robots

    @Property(QObject, constant=True)
    def robotList(self):
        """Followed robots matching robotFilter."""
        return self._robot_list

    @Property(QObject, constant=True)
    def mapRobots(self):
        """Every robot RMF reports, for the map."""
        return self._map_robots

    @Property(QObject, constant=True)
    def attention(self):
        return self._attention

    @Property(QObject, constant=True)
    def tasks(self):
        """Tasks matching the state filter and search, underway first."""
        return self._tasks

    @Property(QObject, constant=True)
    def traffic(self):
        """VDA5050 messages matching trafficFilter, newest first."""
        return self._traffic

    # Values.

    @Property(int, constant=True)
    def refreshPeriodMs(self):
        """How often the panels are refreshed; animations between updates last this long."""
        return self._period_ms

    @Property("QVariantList", notify=robotRowsChanged)
    def robotRows(self):
        return self._values["robotRows"]

    @Property("QVariantList", notify=taskRowsChanged)
    def taskRows(self):
        return self._values["taskRows"]

    @Property("QVariantMap", notify=telemetryChanged)
    def telemetry(self):
        return self._values["telemetry"]

    @Property("QVariantMap", notify=onlineChanged)
    def online(self):
        return self._values["online"]

    @Property("QVariantMap", notify=speedLimitsChanged)
    def speedLimits(self):
        return self._values["speedLimits"]

    @Property("QStringList", notify=robotNamesChanged)
    def robotNames(self):
        return self._values["robotNames"]

    @Property("QStringList", notify=onlineRobotNamesChanged)
    def onlineRobotNames(self):
        return self._values["onlineRobotNames"]

    @Property("QVariantMap", notify=destinationsChanged)
    def activeDestinations(self):
        return self._values["destinations"]

    @Property("QVariantList", notify=routesChanged)
    def routes(self):
        return self._values["routes"]

    @Property(int, notify=summaryChanged)
    def robotCount(self):
        return self._summary["robotCount"]

    @Property(int, notify=summaryChanged)
    def onlineCount(self):
        return self._summary["onlineCount"]

    @Property(str, notify=summaryChanged)
    def fleetSummary(self):
        return self._summary["fleetSummary"]

    @Property(int, notify=summaryChanged)
    def runningTasks(self):
        return self._summary["runningTasks"]

    @Property(int, notify=summaryChanged)
    def queuedTasks(self):
        return self._summary["queuedTasks"]

    @Property(int, notify=summaryChanged)
    def completedTasks(self):
        return self._summary["completedTasks"]

    @Property(int, notify=summaryChanged)
    def taskTotal(self):
        return self._summary["taskTotal"]

    @Property(int, notify=summaryChanged)
    def criticalCount(self):
        return self._summary["criticalCount"]

    @Property(int, notify=summaryChanged)
    def warningCount(self):
        return self._summary["warningCount"]

    @Property(str, notify=summaryChanged)
    def healthLevel(self):
        return self._summary["healthLevel"]

    @Property(str, notify=summaryChanged)
    def healthDetail(self):
        return self._summary["healthDetail"]

    # Filters, set from QML.

    def _get_robot_filter(self):
        return self._robot_filter

    def _set_robot_filter(self, text):
        if text != self._robot_filter:
            self._robot_filter = text
            self._refilter()

    robotFilter = Property(str, _get_robot_filter, _set_robot_filter, notify=filtersChanged)

    def _get_task_state_filter(self):
        return self._task_state_filter

    def _set_task_state_filter(self, state):
        if state != self._task_state_filter:
            self._task_state_filter = state
            self._refilter()

    taskStateFilter = Property(str, _get_task_state_filter, _set_task_state_filter, notify=filtersChanged)

    def _get_task_search(self):
        return self._task_search

    def _set_task_search(self, text):
        if text != self._task_search:
            self._task_search = text
            self._refilter()

    taskSearch = Property(str, _get_task_search, _set_task_search, notify=filtersChanged)

    def _get_traffic_filter(self):
        return self._traffic_filter

    def _set_traffic_filter(self, kind):
        if kind != self._traffic_filter:
            self._traffic_filter = kind
            self._refilter()

    trafficFilter = Property(str, _get_traffic_filter, _set_traffic_filter, notify=filtersChanged)
