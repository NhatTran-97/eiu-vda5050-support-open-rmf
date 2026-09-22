import json
import os
import time
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QCoreApplication

from eiu_fleet_ui.config import RobotIdentity
from eiu_fleet_ui.robot_registry import REPLY_TIMEOUT_SEC, RobotRegistry


def registry_message(fleet, robots):
    return json.dumps({"fleet": fleet, "interface": "AMR", "adapter_node": "adapter", "series": "Burger",
                       "limits": {}, "chargers": [{"name": "charger_1", "used_by": None}],
                       "robots": robots})


def robot(name, serial, source="runtime", retired=False):
    return {"name": name, "manufacturer": "ROBOTIS", "serial": serial, "charger": "charger_1",
            "source": source, "retired": retired}


def discovery_message(reporter, *serials):
    return json.dumps({"reporter": reporter, "interface": "AMR", "robots": [
        {"manufacturer": "ROBOTIS", "serial": s, "series": "Burger", "kinematic": "DIFF", "speed_max": 0.22,
         "pose": None} for s in serials]})


class Recorder:
    """Collects what a registry emits."""

    def __init__(self, registry):
        self.added, self.removed, self.detected, self.checks, self.results, self.changes = [], [], [], [], [], 0
        registry.robotAdded.connect(self.added.append)
        registry.robotRemoved.connect(self.removed.append)
        registry.newRobotsDetected.connect(self.detected.append)
        registry.checkResult.connect(self.checks.append)
        registry.requestResult.connect(self.results.append)
        registry.changed.connect(self._changed)

    def _changed(self):
        self.changes += 1


class RobotRegistryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        configured = [RobotIdentity("tb3_1", "ROBOTIS", "0001", "AMR", "adapter", "tb3_fleet")]
        self.registry = RobotRegistry(configured)
        self.seen = Recorder(self.registry)

    def test_a_registered_robot_is_announced_to_the_backends(self):
        self.registry._on_incoming("registry", registry_message("tb3_fleet", [robot("tb3_2", "0002")]))
        self.assertEqual([r.name for r in self.seen.added], ["tb3_2"])
        self.assertIn("tb3_2", self.registry.fleetsJson)
        self.registry._on_incoming("registry", registry_message("tb3_fleet", [robot("tb3_2", "0002", retired=True)]))
        self.assertEqual(self.seen.removed, ["tb3_2"])

    def test_new_robots_are_announced_once(self):
        self.registry._on_incoming("registry", registry_message("tb3_fleet", [robot("tb3_1", "0001", "config")]))
        self.registry._on_incoming("discovery", discovery_message("tb3_fleet", "0002"))
        self.registry._on_incoming("discovery", discovery_message("tb3_fleet", "0002", "0003"))
        self.assertEqual([json.loads(k) for k in self.seen.detected], [["ROBOTIS/0002"], ["ROBOTIS/0003"]])
        pending = json.loads(self.registry.pendingJson)
        self.assertEqual([p["key"] for p in pending], ["ROBOTIS/0002", "ROBOTIS/0003"])

    def test_dismissed_robots_leave_the_pending_list(self):
        self.registry._on_incoming("registry", registry_message("tb3_fleet", []))
        self.registry._on_incoming("discovery", discovery_message("tb3_fleet", "0002"))
        self.registry.dismiss("ROBOTS/0002")
        self.registry.dismiss("ROBOTIS/0002")
        self.assertEqual(json.loads(self.registry.pendingJson), [])

    def test_undecodable_messages_are_ignored(self):
        self.registry._on_incoming("registry", "{not json")
        self.registry._on_incoming("discovery", "[1, 2")
        self.registry._on_incoming("result", "nope")
        self.assertEqual(self.seen.changes, 0)

    def test_a_result_reaches_the_request_it_answers(self):
        request_id = self.registry.check(json.dumps({"fleet": "tb3_fleet", "name": "tb3_2"}))
        sent = json.loads(self.registry._outgoing.get_nowait()[1])
        self.assertEqual((sent["action"], sent["dry_run"], sent["request_id"]), ("add", True, request_id))

        answer = {"request_id": request_id, "ok": True, "errors": [], "warnings": []}
        self.registry._on_incoming("result", json.dumps(answer))
        self.assertEqual([json.loads(t)["request_id"] for t in self.seen.checks], [request_id])
        self.assertEqual(self.seen.results, [])

        # A second copy or somebody else's result is dropped.
        self.registry._on_incoming("result", json.dumps(answer))
        self.registry._on_incoming("result", json.dumps({"request_id": "other-client", "ok": True}))
        self.assertEqual(len(self.seen.checks), 1)

    def test_register_and_remove_are_not_dry_runs(self):
        register_id = self.registry.register(json.dumps({"fleet": "f", "name": "n", "dry_run": True}))
        remove_id = self.registry.remove("f", "n")
        register = json.loads(self.registry._outgoing.get_nowait()[1])
        remove = json.loads(self.registry._outgoing.get_nowait()[1])
        self.assertEqual((register["action"], register["dry_run"]), ("add", False))
        self.assertEqual((remove["action"], remove["name"], remove["fleet"]), ("remove", "n", "f"))

        self.registry._on_incoming("result", json.dumps({"request_id": register_id, "ok": True}))
        self.registry._on_incoming("result", json.dumps({"request_id": remove_id, "ok": True}))
        self.assertEqual(len(self.seen.results), 2)
        self.assertEqual(self.seen.checks, [])

    def test_bad_request_text_still_gets_an_answer(self):
        request_id = self.registry.register("this is not json")
        sent = json.loads(self.registry._outgoing.get_nowait()[1])
        self.assertEqual(sent["request_id"], request_id)

    def test_a_request_nobody_answers_ends_in_a_failure(self):
        request_id = self.registry.check("{}")
        real = time.monotonic
        try:
            time.monotonic = lambda: real() + REPLY_TIMEOUT_SEC + 1
            self.registry._expire_requests()
        finally:
            time.monotonic = real
        result = json.loads(self.seen.checks[0])
        self.assertEqual((result["request_id"], result["ok"], result["errors"][0]["code"]), (request_id, False, "no_reply"))

    def test_without_a_listening_adapter_the_request_fails_at_once(self):
        request_id = self.registry.register("{}")
        self.registry._drain()   # no publisher: attach() was never called
        result = json.loads(self.seen.results[0])
        self.assertEqual((result["request_id"], result["errors"][0]["code"]), (request_id, "no_adapter"))

    def test_suggestions_are_served_to_the_form(self):
        self.registry._on_incoming("registry", registry_message("tb3_fleet", [robot("tb3_1", "0001", "config")]))
        suggestion = json.loads(self.registry.suggestFor("tb3_fleet", "ROBOTIS", "0002"))
        self.assertEqual((suggestion["name"], suggestion["charger"]), ("tb3_2", "charger_1"))
        self.assertEqual(self.registry.sourceOf("tb3_1"), "config")


if __name__ == "__main__":
    unittest.main()
