import json
import os
import unittest
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QCoreApplication

from eiu_fleet_ui.config import FleetConfig, RobotIdentity
from eiu_fleet_ui.dashboard import Dashboard
from eiu_fleet_ui.mqtt_client import MqttClient, MqttSnapshot


class FakeRos:
    def __init__(self):
        self.rows, self.tasks, self.version = [], [], 0
        self.rmfOnline, self.activeConflicts, self.blockedLanes = True, 0, 0

    def robots_snapshot(self):
        return list(self.rows)

    def stale_fleets(self, now=None):
        return []

    def tasks_snapshot(self):
        return self.version, [dict(t) for t in self.tasks]


class FakeMqtt:
    def __init__(self):
        self.connected = True
        self.online, self.telemetry, self.traffic, self.traffic_version = {}, {}, [], 0

    def snapshot(self, now=None):
        return MqttSnapshot(dict(self.online), dict(self.telemetry), list(self.traffic), self.traffic_version)


def robot(name, x=1.0, task=""):
    return {"key": f"f/{name}", "name": name, "fleet": "f", "model": "", "status": "MOVING", "battery": 60.0,
            "level": "L1", "task": task, "x": x, "y": 0.0, "yaw": 0.0, "path": []}


class DashboardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.ros, self.mqtt = FakeRos(), FakeMqtt()
        settings = SimpleNamespace(known_robots=lambda: [("a", "f"), ("b", "f")], fleetName="f",
                                   broker_conflicts=lambda: [])
        self.dashboard = Dashboard(
            settings, SimpleNamespace(waypoints=lambda: [{"name": "wp", "x": 0.0, "y": 0.0}]),
            self.ros, self.mqtt, SimpleNamespace(speed_limits=lambda: {"a": 0.0}),
            SimpleNamespace(conflict_list=lambda: [], pending_list=lambda: []),
            SimpleNamespace(attention_list=lambda: []), period_s=0.2)
        self.signals = []
        for name in ("summaryChanged", "robotRowsChanged", "taskRowsChanged", "telemetryChanged",
                     "onlineChanged", "routesChanged"):
            getattr(self.dashboard, name).connect(lambda n=name: self.signals.append(n))

    def test_a_refresh_with_nothing_new_notifies_nothing(self):
        self.ros.rows = [robot("a")]
        self.mqtt.online = {"a": True, "b": False}
        self.dashboard.refresh()
        self.signals.clear()
        changes = []
        self.dashboard.robotList.dataChanged.connect(lambda *_: changes.append(1))
        self.dashboard.attention.dataChanged.connect(lambda *_: changes.append(1))
        self.dashboard.refresh()
        self.assertEqual((self.signals, changes), ([], []))

    def test_a_moving_robot_changes_its_row_only(self):
        self.ros.rows = [robot("a"), robot("b")]
        self.dashboard.refresh()
        changed = []
        self.dashboard.robotList.dataChanged.connect(lambda top, *_: changed.append(top.row()))
        self.ros.rows = [robot("a", x=2.0), robot("b")]
        self.dashboard.refresh()
        self.assertEqual(changed, [0])
        self.assertEqual(self.dashboard.robots.get(0)["x"], 2.0)

    def test_panels_and_summary(self):
        self.ros.rows = [robot("a", task="t1")]
        self.ros.tasks = [{"id": "r1", "rmf_id": "t1", "state": "underway", "destination": "wp", "robot": "a"},
                          {"id": "r2", "rmf_id": "", "state": "queued", "destination": "wp", "robot": "—"}]
        self.ros.version = 1
        self.mqtt.online = {"a": True, "b": False}
        self.dashboard.refresh()
        d = self.dashboard
        self.assertEqual((d.robotCount, d.onlineCount, d.runningTasks, d.queuedTasks, d.taskTotal), (2, 1, 1, 1, 2))
        self.assertEqual(d.robotNames, ["a", "b"])
        self.assertEqual(d.onlineRobotNames, ["a"])
        self.assertEqual(d.activeDestinations, {"wp": True})
        self.assertEqual([d.attention.get(i)["key"] for i in range(d.attention.count)], ["robot:b:offline"])
        self.assertEqual((d.healthLevel, d.healthDetail), ("DEGRADED", "b VDA5050 offline"))
        self.assertEqual(d.robotList.get(0)["task_destination"], "wp")
        self.assertEqual(d.speedLimits, {"a": 0.0})

    def test_filters_change_the_lists_at_once(self):
        self.ros.tasks = [{"id": "r1", "state": "completed", "robot": "a", "destination": "wp"},
                          {"id": "r2", "state": "failed", "robot": "b", "destination": "dock"}]
        self.ros.version = 1
        self.mqtt.traffic = [{"id": 2, "type": "state"}, {"id": 1, "type": "order"}]
        self.mqtt.traffic_version = 1
        self.dashboard.refresh()
        d = self.dashboard
        d.robotFilter = "B"
        d.taskStateFilter = "Failed"
        d.trafficFilter = "order"
        self.assertEqual((d.robotList.count, d.robotList.get(0)["name"]), (1, "b"))
        self.assertEqual((d.tasks.count, d.tasks.get(0)["id"]), (1, "r2"))
        self.assertEqual((d.traffic.count, d.traffic.get(0)["id"]), (1, 1))
        d.taskStateFilter = "All"
        d.taskSearch = "dock"
        self.assertEqual(d.tasks.count, 1)

    def test_tasks_are_read_again_only_when_their_version_changes(self):
        self.ros.tasks = [{"id": "r1", "state": "queued", "robot": "—", "destination": "wp"}]
        self.dashboard.refresh()
        self.ros.tasks.append({"id": "r2", "state": "queued", "robot": "—", "destination": "wp"})
        self.dashboard.refresh()
        self.assertEqual(self.dashboard.tasks.count, 1, "the same version is not read again")
        self.ros.version = 1
        self.dashboard.refresh()
        self.assertEqual(self.dashboard.tasks.count, 2)


def config(*robots):
    return FleetConfig(fleet_name="f", fleet_names=("f",), interface_name="AMR", broker_host="localhost",
                       broker_port=1, username=None, password=None, robots=tuple(robots),
                       task_categories=("patrol",), nav_graph=None, websocket_uri=None, source="test")


class MqttSnapshotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.robot = RobotIdentity("a", "M", "1", "AMR", "adapter", "f")
        self.other = RobotIdentity("b", "M", "2", "AMR", "adapter", "f")
        self.mqtt = MqttClient(config(self.robot, self.other))

    def send(self, robot, leaf, payload):
        self.mqtt._on_message(None, None, SimpleNamespace(topic=robot.topic(leaf), payload=json.dumps(payload).encode()))

    def test_only_robots_with_new_state_are_exported_again(self):
        self.send(self.robot, "state", {"orderId": "o1"})
        self.send(self.other, "state", {"orderId": "o2"})
        first = self.mqtt.snapshot()
        self.send(self.robot, "state", {"orderId": "o3"})
        second = self.mqtt.snapshot()
        self.assertIs(second.telemetry["b"], first.telemetry["b"])
        self.assertEqual(second.telemetry["a"]["order_id"], "o3")
        self.assertIs(self.mqtt.snapshot().telemetry["a"], second.telemetry["a"])

    def test_the_log_keeps_its_version_until_a_message_is_logged(self):
        self.send(self.robot, "connection", {"connectionState": "ONLINE"})
        first = self.mqtt.snapshot()
        self.assertEqual([e["type"] for e in first.traffic], ["connection"])
        self.assertIs(self.mqtt.snapshot().traffic, first.traffic)
        self.send(self.robot, "order", {"orderId": "o1", "orderUpdateId": 0, "nodes": [], "edges": []})
        second = self.mqtt.snapshot()
        self.assertGreater(second.traffic_version, first.traffic_version)
        self.assertEqual([e["type"] for e in second.traffic], ["order", "connection"])
        self.assertNotIn("raw", second.traffic[0])

    def test_an_instant_action_takes_the_status_the_robot_reports(self):
        self.send(self.robot, "instantActions", {"actions": [{"actionId": "p1", "actionType": "startPause",
                                                              "blockingType": "HARD"}]})
        self.assertEqual(self.mqtt.snapshot().traffic[0]["status"], "SENT")
        self.send(self.robot, "state", {"actionStates": [{"actionId": "p1", "actionStatus": "RUNNING"}]})
        self.assertEqual(self.mqtt.snapshot().traffic[-1]["status"], "RUNNING")
        self.send(self.robot, "state", {"actionStates": [{"actionId": "p1", "actionStatus": "FINISHED"}]})
        log = self.mqtt.snapshot().traffic
        self.assertEqual([e["status"] for e in log if e["type"] == "instantAction"], ["FINISHED"])
        self.assertEqual(self.mqtt._open_instant["a"], [], "a finished action is no longer followed")
        blocking = self.mqtt.snapshot().telemetry["a"]["action_states"][0]["blockingType"]
        self.assertEqual(blocking, "HARD")


if __name__ == "__main__":
    unittest.main()
