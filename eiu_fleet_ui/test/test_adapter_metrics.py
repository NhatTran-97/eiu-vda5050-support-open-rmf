import json
import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QCoreApplication

from eiu_fleet_ui.adapter_metrics import AdapterMetrics
from eiu_fleet_ui.config import RobotIdentity


def identity(name, node, fleet):
    return RobotIdentity(name=name, manufacturer="ROBOTIS", serial=name, interface_name="AMR", adapter_node=node, fleet_name=fleet)


def report(dropped=0, connected=True):
    return json.dumps({
        "fleet": "tb3_fleet", "interval_s": 60.0, "uptime_s": 10.0,
        "robots": {"registered": 1, "online": 1, "state_age_max_s": 0.5, "oldest_state_robot": "tb3_1", "state_timeout_s": 10.0},
        "rx": {"state": 10, "visualization": 20, "connection": 1, "factsheet": 0, "unregistered": 0},
        "dropped": {"bad_payload": dropped, "bad_topic": 0, "invalid_state": 0, "stale_state": 0, "oversize": 0},
        "published": {"ok": 1, "failed": 0}, "mqtt": {"connected": connected, "connections_lost": 0},
        "latency_us": {"handle_state": {"count": 1, "mean": 1, "p50": 1, "p99": 1, "max": 1}},
        "update_loop": {"pass_us": {"count": 1, "mean": 1, "p50": 1, "p99": 1, "max": 1}, "overruns": 0, "period_ms": 100.0},
    })


class AdapterMetricsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.metrics = AdapterMetrics([identity("tb3_1", "adapter_tb3", "tb3_fleet")])

    def adapters(self):
        return json.loads(self.metrics.metricsJson)["adapters"]

    def test_a_configured_adapter_is_listed_before_it_reports(self):
        adapters = self.adapters()
        self.assertEqual([a["node"] for a in adapters], ["adapter_tb3"])
        self.assertEqual(adapters[0]["status"], "waiting")
        self.assertEqual(json.loads(self.metrics.attentionJson), [])

    def test_a_report_updates_the_snapshot(self):
        self.metrics._on_incoming("adapter_tb3", report())
        adapter = self.adapters()[0]
        self.assertEqual(adapter["status"], "ok")
        self.assertEqual(adapter["robots"]["registered"], 1)

    def test_a_report_that_is_not_json_is_ignored(self):
        self.metrics._on_incoming("adapter_tb3", "{not json")
        self.assertEqual(self.adapters()[0]["status"], "waiting")

    def test_attention_items_change_only_when_the_problems_do(self):
        changes = []
        self.metrics.attentionChanged.connect(lambda: changes.append(1))
        self.metrics._on_incoming("adapter_tb3", report())
        self.metrics._on_incoming("adapter_tb3", report())
        self.assertEqual(changes, [])                       # nothing wrong, nothing to announce
        self.metrics._on_incoming("adapter_tb3", report(dropped=3))
        self.assertEqual(len(changes), 1)
        self.assertIn("dropped 3", json.loads(self.metrics.attentionJson)[0]["title"])
        self.metrics._on_incoming("adapter_tb3", report(dropped=3))
        self.assertEqual(len(changes), 2)                   # the problem is gone
        self.assertEqual(json.loads(self.metrics.attentionJson), [])

    def test_the_snapshot_signal_fires_with_each_report(self):
        changes = []
        self.metrics.changed.connect(lambda: changes.append(1))
        self.metrics._on_incoming("adapter_tb3", report())
        self.assertEqual(len(changes), 1)

    def test_an_adapter_of_a_robot_added_later_is_followed(self):
        self.metrics.add_adapter(identity("amr_2", "adapter_amr", "amr_fleet"))
        self.metrics._refresh()
        self.assertEqual([a["node"] for a in self.adapters()], ["adapter_tb3", "adapter_amr"])

    def test_a_robot_of_an_adapter_already_followed_adds_nothing(self):
        self.metrics.add_adapter(identity("tb3_2", "adapter_tb3", "tb3_fleet"))
        self.assertEqual(len(self.adapters()), 1)


class Presence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def test_the_summary_follows_which_adapters_publish(self):
        metrics = AdapterMetrics([identity("tb3_1", "adapter_tb3", "tb3_fleet"), identity("amr_1", "adapter_amr", "amr_fleet")])
        self.assertEqual((metrics.adaptersTotal, metrics.adaptersFound, metrics.adaptersLevel), (2, 0, "wait"))
        changes = []
        metrics.summaryChanged.connect(lambda: changes.append(1))
        metrics._present["adapter_amr"] = True
        metrics._present["adapter_tb3"] = False
        metrics._refresh()
        self.assertEqual(metrics.adaptersFound, 1)
        self.assertEqual(len(changes), 1)
        metrics._refresh()
        self.assertEqual(len(changes), 1)                   # unchanged, so no signal


if __name__ == "__main__":
    unittest.main()
