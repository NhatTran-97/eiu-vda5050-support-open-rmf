import os
import queue
import unittest
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QCoreApplication

from eiu_fleet_ui import ros_control
from eiu_fleet_ui.config import FleetConfig, RobotIdentity
from eiu_fleet_ui.ros_control import RosControl


def config(*robots):
    return FleetConfig(fleet_name="f", fleet_names=("f",), interface_name="AMR", broker_host="localhost",
                       broker_port=1, username=None, password=None, robots=tuple(robots),
                       task_categories=("patrol",), nav_graph=None, websocket_uri=None, source="test")


class RosControlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.control = RosControl(config(RobotIdentity("tb3_1", "M", "1", "AMR", "adapter", "f")))
        self.results = []
        self.control.commandResult.connect(lambda *r: self.results.append(r))

    def queued(self):
        out = []
        while True:
            try:
                out.append(self.control._command_queue.get_nowait()[:2])
            except queue.Empty:
                return out

    def test_a_command_waits_until_the_adapter_answers(self):
        self.control.pauseRobot("tb3_1")
        self.assertEqual(self.queued(), [("pause", "tb3_1")])
        self.assertEqual(self.control.pending, {"tb3_1/pause": True})

        self.control._result.emit("tb3_1", "pause", True, "paused")   # as the ROS thread does
        self.assertEqual(self.results, [], "results reach QML on the GUI thread")
        self.app.processEvents()
        self.assertEqual(self.results, [("tb3_1", "pause", True, "paused")])
        self.assertEqual(self.control.pending, {})

    def test_the_same_command_is_not_sent_twice_while_it_waits(self):
        self.control.setSpeedLimit("tb3_1", 0.3)
        self.control.setSpeedLimit("tb3_1", 0.2)
        self.control.resumeRobot("tb3_1")
        self.assertEqual(self.queued(), [("speed_limit", "tb3_1"), ("resume", "tb3_1")])
        self.assertEqual(len(self.results), 1)
        self.assertFalse(self.results[0][2])
        self.assertIn("still waiting", self.results[0][3])

    def test_a_command_nobody_answers_fails_after_the_timeout(self):
        self.control.initPosition("tb3_1", 1.0, 2.0, 0.0)
        with mock.patch.object(ros_control.time, "monotonic", return_value=1e12):
            self.control._expire()
        self.assertEqual(len(self.results), 1)
        robot, action, ok, message = self.results[0]
        self.assertEqual((robot, action, ok), ("tb3_1", "init_position", False))
        self.assertIn("No answer", message)
        self.assertEqual(self.control.pending, {})
        self.control.initPosition("tb3_1", 1.0, 2.0, 0.0)
        self.assertEqual(self.control.pending, {"tb3_1/init_position": True}, "it can be sent again")

    def test_a_late_answer_is_still_reported(self):
        self.control._result.emit("tb3_1", "init_position", True, "ok")
        self.app.processEvents()
        self.assertEqual(self.results, [("tb3_1", "init_position", True, "ok")])

    def test_robots_of_one_adapter_share_its_parameter_client(self):
        self.control.add_robot(RobotIdentity("tb3_2", "M", "2", "AMR", "adapter", "f"))
        self.control.add_robot(RobotIdentity("amr_1", "N", "3", "AMR", "other_adapter", "g"))
        self.assertEqual(self.control._adapter_of("tb3_2"), "adapter")
        self.assertEqual(self.control._adapter_of("amr_1"), "other_adapter")
        self.assertIsNone(self.control._adapter_of("ghost"))
        self.assertEqual(self.control.speed_limits(), {"tb3_1": 0.0, "tb3_2": 0.0, "amr_1": 0.0})


if __name__ == "__main__":
    unittest.main()
