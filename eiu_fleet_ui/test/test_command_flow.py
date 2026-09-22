import json
import os
import queue
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["EIU_TASK_CACHE_DELAY"] = "0.05"

from PySide6.QtCore import QCoreApplication

from eiu_fleet_ui import ros_bridge
from eiu_fleet_ui.ros_bridge import RosBridge


def response(request_id, body):
    return SimpleNamespace(request_id=request_id, json_msg=json.dumps(body))


class CommandFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.env = mock.patch.dict(os.environ, {"EIU_CONFIG_DIR": self.dir.name})
        self.env.start()
        self.bridge = RosBridge()
        self.bridge._ok = True
        self.bridge._task_pub = object()
        self.bridge._robots_json = json.dumps([{"name": "robot_a", "x": 0, "y": 0}])
        self.bridge._robot_fleet = {"robot_a": "fleet_a"}
        self.results = []
        self.paths = []
        self.bridge.dispatchResult.connect(lambda *r: self.results.append(r))
        self.bridge.pathChanged.connect(lambda: self.paths.append(1))

    def tearDown(self):
        self.bridge._cache_writer.close()
        self.env.stop()
        self.dir.cleanup()

    def sent(self):
        out = []
        while True:
            try:
                out.append(self.bridge._command_queue.get_nowait())
            except queue.Empty:
                return out

    def settle(self):
        self.app.processEvents()
        return list(self.results)

    def start_task(self, rmf_id="task.1", state="underway"):
        request_id = self.bridge.dispatch("patrol", "wp", 1)
        self.sent()
        self.bridge._attach_rmf_id(request_id, rmf_id, True)
        self.bridge._tasks[0]["state"] = state
        self.settle()
        self.results.clear()
        return request_id

    # Dispatch and delivery.

    def test_dispatch_queues_the_request_and_answers_with_its_id(self):
        request_id = self.bridge.dispatch("patrol", "wp", 1)
        self.assertEqual([(r, e) for r, e in [(c[0], json.loads(c[1])["type"]) for c in self.sent()]],
                         [(request_id, "dispatch_task_request")])
        self.assertEqual(self.settle(), [(request_id, "dispatch", True, "Sent to the dispatcher")])
        self.assertEqual(self.bridge._tasks[0]["state"], "queued")
        self.assertEqual(self.paths, [1])

    def test_a_direct_request_names_the_robot_and_its_fleet(self):
        self.bridge.dispatchToRobot("patrol", "wp", 1, "robot_a")
        envelope = json.loads(self.sent()[0][1])
        self.assertEqual((envelope["type"], envelope["robot"], envelope["fleet"]),
                         ("robot_task_request", "robot_a", "fleet_a"))

    def test_nothing_is_sent_when_ros_is_not_ready_or_the_robot_is_unknown(self):
        unknown = self.bridge.dispatchToRobot("patrol", "wp", 1, "ghost")
        self.bridge._task_pub = None
        offline = self.bridge.dispatch("patrol", "wp", 1)
        results = self.settle()
        self.assertEqual([(r[0], r[2]) for r in results], [(unknown, False), (offline, False)])
        self.assertEqual(self.sent(), [])
        self.assertEqual(self.bridge._tasks, [])

    def test_delivery_answers_and_refreshes_the_planned_path(self):
        request_id = self.bridge.dispatchDelivery("p", "dispenser_x", "d", "ingestor_y", "goods", 3)
        envelope = json.loads(self.sent()[0][1])
        description = envelope["request"]["description"]
        self.assertEqual(description["pickup"]["payload"], {"sku": "goods", "quantity": 3})
        self.assertEqual((description["pickup"]["handler"], description["dropoff"]["handler"]),
                         ("dispenser_x", "ingestor_y"))
        self.assertEqual(self.settle(), [(request_id, "dispatch", True, "Sent to the dispatcher")])
        self.assertEqual(self.paths, [1])
        self.assertEqual(self.bridge.plannedDest, "d")
        self.assertEqual(self.bridge._tasks[0]["pickup_handler"], "dispenser_x")

    def test_a_delivery_sent_to_a_robot_names_it_and_its_fleet(self):
        self.bridge.dispatchDeliveryToRobot("p", "dispenser_x", "d", "ingestor_y", "goods", 3, "robot_a")
        envelope = json.loads(self.sent()[0][1])
        self.assertEqual((envelope["type"], envelope["robot"], envelope["fleet"]),
                         ("robot_task_request", "robot_a", "fleet_a"))
        self.assertEqual(envelope["request"]["description"]["pickup"]["payload"], {"sku": "goods", "quantity": 3})

    def test_a_delivery_sent_to_an_unknown_robot_is_refused(self):
        request_id = self.bridge.dispatchDeliveryToRobot("p", "a", "d", "b", "s", 1, "ghost")
        self.assertEqual(self.sent(), [])
        self.assertEqual([(r[0], r[2]) for r in self.settle()], [(request_id, False)])

    def test_each_command_gets_its_own_id(self):
        ids = {self.bridge.dispatch("patrol", "wp", 1), self.bridge.dispatchDelivery("p", "a", "d", "b", "s", 1)}
        self.assertEqual(len(ids), 2)

    def test_a_queued_task_nobody_answers_fails(self):
        self.bridge.dispatch("patrol", "wp", 1)
        with mock.patch.object(ros_bridge, "DISPATCH_TIMEOUT_SEC", -1):
            self.bridge._check_dispatch_timeouts()
        self.assertEqual((self.bridge._tasks[0]["state"], self.bridge._tasks[0]["error"]),
                         ("failed", "No response from dispatcher"))

    # Cancel.

    def test_cancel_is_not_done_until_rmf_confirms(self):
        self.start_task()
        request_id = self.bridge.cancel_task("task.1")
        sent = self.sent()
        self.assertEqual((sent[0][0], json.loads(sent[0][1])["task_id"]), (request_id, "task.1"))
        task = self.bridge._tasks[0]
        self.assertEqual((task["state"], task["cancel"]), ("underway", "requested"))
        self.assertEqual(self.settle(), [], "no result before RMF answers")

        self.bridge._on_task_response(response(request_id, {"success": True}))
        self.assertEqual(self.settle(), [(request_id, "cancel", True, "Cancelled")])
        self.assertEqual(self.bridge._tasks[0]["state"], "cancelled")
        self.assertNotIn("cancel", self.bridge._tasks[0])

    def test_a_second_answer_changes_nothing(self):
        self.start_task()
        request_id = self.bridge.cancel_task("task.1")
        self.bridge._on_task_response(response(request_id, {"success": True}))
        self.bridge._on_task_response(response(request_id, {"success": False}))
        self.assertEqual(len(self.settle()), 1)
        self.assertEqual(self.bridge._tasks[0]["state"], "cancelled")

    def test_a_refused_cancel_keeps_the_task_running_and_can_be_retried(self):
        self.start_task()
        request_id = self.bridge.cancel_task("task.1")
        self.bridge._on_task_response(response(request_id, {"success": False, "errors": [{"detail": "too late"}]}))
        self.assertEqual(self.settle(), [(request_id, "cancel", False, "too late")])
        task = self.bridge._tasks[0]
        self.assertEqual((task["state"], task["cancel"], task["cancel_error"]), ("underway", "failed", "too late"))
        again = self.bridge.cancel_task("task.1")
        self.assertNotEqual(again, request_id)
        self.assertEqual(self.bridge._tasks[0]["cancel"], "requested")

    def test_a_cancel_rmf_never_answers_ends_in_a_failure(self):
        self.start_task()
        with mock.patch.object(ros_bridge, "DISPATCH_TIMEOUT_SEC", -1):
            request_id = self.bridge.cancel_task("task.1")
            self.bridge._check_dispatch_timeouts()
        self.assertEqual(self.settle(), [(request_id, "cancel", False, "No answer from the dispatcher")])
        self.assertEqual(self.bridge._tasks[0]["cancel"], "failed")
        # An answer that turns up late is ignored.
        self.bridge._on_task_response(response(request_id, {"success": True}))
        self.assertEqual(self.bridge._tasks[0]["state"], "underway")
        self.assertEqual(len(self.settle()), 1)

    def test_the_task_state_can_settle_a_cancel_before_the_answer(self):
        self.start_task()
        request_id = self.bridge.cancel_task("task.1")
        self.bridge._tasks[0]["state"] = "cancelled"
        self.bridge._check_dispatch_timeouts()
        self.assertEqual(self.settle(), [(request_id, "cancel", True, "Cancelled")])

    def test_a_task_that_finished_meanwhile_reports_that(self):
        self.start_task()
        request_id = self.bridge.cancel_task("task.1")
        self.bridge._tasks[0]["state"] = "completed"
        self.bridge._check_dispatch_timeouts()
        self.assertEqual(self.settle(), [(request_id, "cancel", False, "Task already completed")])
        self.assertNotIn("cancel", self.bridge._tasks[0])

    def test_cancel_is_refused_for_unknown_finished_or_pending_tasks(self):
        self.start_task(state="completed")
        finished = self.bridge.cancel_task("task.1")
        unknown = self.bridge.cancel_task("nope")
        no_id = self.bridge.cancel_task("")
        self.bridge._tasks[0]["state"] = "underway"
        self.bridge.cancel_task("task.1")
        twice = self.bridge.cancel_task("task.1")
        results = self.settle()
        self.assertEqual([(r[0], r[2]) for r in results], [(finished, False), (unknown, False), (no_id, False), (twice, False)])
        self.assertEqual(len(self.sent()), 1, "only the one valid cancel was sent")

    def test_an_answer_to_someone_elses_request_is_ignored(self):
        self.start_task()
        self.bridge._on_task_response(response("other-client", {"success": True}))
        self.assertEqual(self.bridge._tasks[0]["state"], "underway")
        self.assertEqual(self.settle(), [])

    # Task errors.

    def test_error_detail_reads_json_encoded_and_plain_entries(self):
        self.assertEqual(ros_bridge._error_detail({"detail": "no route", "category": "negotiation"}), "no route")
        self.assertEqual(ros_bridge._error_detail({"category": "negotiation"}), "negotiation")
        self.assertEqual(ros_bridge._error_detail(
            json.dumps({"category": "negotiation", "affected_tasks": ["task.1-13"]})), "negotiation")
        self.assertEqual(ros_bridge._error_detail("plain text"), "plain text")

    def test_dispatch_state_error_shows_the_detail_not_raw_json(self):
        self.start_task()
        # Still bidding -- no robot assigned yet -- so the error is live, not stale.
        state = SimpleNamespace(task_id="task.1", status=3,
                                assignment=SimpleNamespace(is_assigned=False, expected_robot_name=""),
                                errors=[json.dumps({"category": "negotiation", "detail": "no route",
                                                    "affected_tasks": ["task.1-13"]})])
        self.bridge._on_dispatch_states(SimpleNamespace(active=[state], finished=[]))
        self.assertEqual(self.bridge._tasks[0]["error"], "no route")

        # The next update reports no errors -- the stale one is cleared, not kept.
        state.errors = []
        self.bridge._on_dispatch_states(SimpleNamespace(active=[state], finished=[]))
        self.assertEqual(self.bridge._tasks[0]["error"], "")

    def test_dispatch_state_error_does_not_survive_a_robot_assignment(self):
        self.start_task()
        state = SimpleNamespace(task_id="task.1", status=3,
                                assignment=SimpleNamespace(is_assigned=False, expected_robot_name=""),
                                errors=[json.dumps({"category": "negotiation", "detail": "planner retry"})])
        self.bridge._on_dispatch_states(SimpleNamespace(active=[state], finished=[]))
        self.assertEqual(self.bridge._tasks[0]["error"], "planner retry")

        # A robot gets assigned -- the earlier bidding error is stale from here on,
        # even though RMF keeps reporting it for the task's whole lifetime.
        state.assignment = SimpleNamespace(is_assigned=True, expected_robot_name="robot_a")
        self.bridge._on_dispatch_states(SimpleNamespace(active=[state], finished=[]))
        self.assertEqual(self.bridge._tasks[0]["error"], "")

        self.bridge._on_dispatch_states(SimpleNamespace(active=[state], finished=[]))
        self.assertEqual(self.bridge._tasks[0]["error"], "")

    def test_task_response_state_update_clears_a_stale_error_on_completion(self):
        self.start_task()
        self.bridge._tasks[0]["error"] = "[TaskPlanner] Failed to compute assignments for task_id [task.1]"
        self.bridge._on_task_response(response("x", {
            "type": "task_state_update",
            "task": {"booking": {"id": "task.1"}, "status": "completed"},
        }))
        self.assertEqual(self.bridge._tasks[0]["state"], "completed")
        self.assertEqual(self.bridge._tasks[0]["error"], "")

    def test_a_stale_error_does_not_survive_into_a_completed_task(self):
        self.start_task()
        self.bridge.apply_task_state_update({
            "booking": {"id": "task.1"}, "status": "underway",
            "dispatch": {"errors": [{"category": "negotiation", "detail": "retrying"}]},
        })
        self.assertEqual(self.bridge._tasks[0]["error"], "retrying")

        self.bridge.apply_task_state_update({"booking": {"id": "task.1"}, "status": "completed"})
        self.assertEqual(self.bridge._tasks[0]["state"], "completed")
        self.assertEqual(self.bridge._tasks[0]["error"], "")

    # Task cache.

    def test_a_damaged_task_cache_gives_an_empty_table(self):
        path = self.bridge._cache_path
        path.parent.mkdir(parents=True, exist_ok=True)
        for content in ("{broken", '{"not": "a list"}', '[1, "x", null]', ""):
            path.write_text(content)
            self.assertEqual(RosBridge._load_tasks(path), [], content)
        path.write_text(json.dumps([{"id": "a", "state": "underway", "cancel": "requested", "cancel_id": "c"}, "junk"]))
        loaded = RosBridge._load_tasks(path)
        self.assertEqual([t["id"] for t in loaded], ["a"])
        self.assertNotIn("cancel", loaded[0])
        self.assertEqual(loaded[0]["robot"], "—")

    def test_task_changes_are_written_once_after_the_delay(self):
        for place in ("a", "b", "c"):
            self.bridge.dispatch("patrol", place, 1)
        self.assertFalse(self.bridge._cache_path.exists(), "nothing is written on the calling thread")
        deadline = time.monotonic() + 3
        while not self.bridge._cache_path.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        time.sleep(0.15)
        saved = json.loads(self.bridge._cache_path.read_text())
        self.assertEqual([t["destination"] for t in saved], ["c", "b", "a"])
        self.assertEqual([p.name for p in self.bridge._cache_path.parent.iterdir()], [self.bridge._cache_path.name])

    def test_shutdown_writes_what_is_still_queued(self):
        self.bridge.dispatch("patrol", "wp", 1)
        self.bridge._cache_writer.flush()
        self.assertEqual(len(json.loads(self.bridge._cache_path.read_text())), 1)

    # Workcells.

    def test_workcells_are_listed_by_kind_as_rmf_reports_them(self):
        seen = []
        self.bridge.workcellsChanged.connect(lambda: seen.append(1))
        msg = lambda guid: SimpleNamespace(guid=guid, request_guid_queue=[], seconds_remaining=0.0)
        self.bridge._on_workcell_state(msg("disp_b"), "dispensers")
        self.bridge._on_workcell_state(msg("disp_a"), "dispensers")
        self.bridge._on_workcell_state(msg("ing_1"), "ingestors")
        self.bridge._on_workcell_state(msg("ing_1"), "ingestors")
        self.assertEqual(json.loads(self.bridge.workcellsJson),
                         {"dispensers": ["disp_a", "disp_b"], "ingestors": ["ing_1"]})
        self.assertEqual(len(seen), 3, "a repeated state is not a change")


if __name__ == "__main__":
    unittest.main()
