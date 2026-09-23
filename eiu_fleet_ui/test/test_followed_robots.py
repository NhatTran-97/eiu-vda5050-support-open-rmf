import json
import os
import time
import unittest
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QCoreApplication, QUrl

from eiu_fleet_ui.config import FleetConfig, FleetSettings, RobotIdentity
from eiu_fleet_ui import mqtt_client as mqtt_client_module
from eiu_fleet_ui.mqtt_client import MqttClient
from eiu_fleet_ui.ros_control import RosControl


def config(*robots):
    return FleetConfig(fleet_name="tb3_fleet", fleet_names=("tb3_fleet",), interface_name="AMR",
                       broker_host="localhost", broker_port=1, username=None, password=None,
                       robots=tuple(robots), task_categories=("patrol",), nav_graph=None,
                       websocket_uri=None, source="test")


def identity(name, serial, fleet="tb3_fleet", maker="ROBOTIS"):
    return RobotIdentity(name, maker, serial, "AMR", "adapter", fleet)


class FollowedRobotsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def test_settings_list_robots_as_they_come_and_go(self):
        settings = FleetSettings(config(identity("tb3_1", "0001")),
                                 icon_for=lambda r: QUrl.fromLocalFile(f"/icons/{r.manufacturer}.png"))
        changes = []
        settings.robotsChanged.connect(lambda: changes.append(1))

        settings.add_robot(identity("amr_1", "0007", "amr_fleet", "EIU-FABLAB"))
        settings.add_robot(identity("amr_1", "0007", "amr_fleet", "EIU-FABLAB"))
        self.assertEqual(settings.robotNamesJson, '["tb3_1", "amr_1"]')
        self.assertEqual(settings.robotFleetsJson, '{"tb3_1": "tb3_fleet", "amr_1": "amr_fleet"}')
        self.assertEqual(settings.robotIconUrls["amr_1"].toLocalFile(), "/icons/EIU-FABLAB.png")
        self.assertEqual(len(changes), 1, "adding the same robot twice changes nothing")

        settings.remove_robot("tb3_1")
        settings.remove_robot("ghost")
        self.assertEqual(settings.robotNamesJson, '["amr_1"]')
        self.assertEqual(settings.primaryRobot, "amr_1")
        self.assertEqual(len(changes), 2)

    def test_mqtt_follows_and_forgets_robots(self):
        mqtt = MqttClient(config(identity("tb3_1", "0001")))
        subscribed = []
        mqtt._client.subscribe = lambda topic, *a, **k: subscribed.append(topic)
        mqtt._client.unsubscribe = lambda topic, *a, **k: subscribed.remove(topic)

        mqtt.add_robot(identity("tb3_2", "0002"))
        self.assertIn("tb3_2", mqtt._online)
        self.assertEqual(subscribed, [], "nothing to subscribe while the broker is not connected")

        mqtt._connected = True
        mqtt.add_robot(identity("tb3_3", "0003"))
        self.assertEqual(sorted(t.rsplit("/", 1)[1] for t in subscribed),
                         ["connection", "instantActions", "order", "state"])
        self.assertIs(mqtt._robot_for_topic("AMR/v2/ROBOTIS/0003/state").name == "tb3_3", True)

        mqtt._telemetry["tb3_3"] = object()
        mqtt.remove_robot("tb3_3")
        self.assertEqual(subscribed, [])
        self.assertNotIn("tb3_3", mqtt._online)
        self.assertNotIn("tb3_3", mqtt._telemetry)
        self.assertIsNone(mqtt._robot_for_topic("AMR/v2/ROBOTIS/0003/state"))
        mqtt.remove_robot("tb3_3")   # a second removal is harmless

    def test_stale_telemetry_reads_offline_even_without_a_final_offline_message(self):
        mqtt = MqttClient(config(identity("tb3_1", "0001")))
        robot = mqtt._robots[0]

        def send(leaf, payload):
            msg = SimpleNamespace(topic=robot.topic(leaf), payload=json.dumps(payload).encode())
            mqtt._on_message(None, None, msg)

        send("connection", {"connectionState": "ONLINE"})
        send("state", {})
        self.assertEqual(mqtt.snapshot().online, {"tb3_1": True})

        # The process died without ever publishing OFFLINE -- telemetry just goes quiet.
        mqtt._last_state_rx["tb3_1"] -= mqtt_client_module.STATE_STALE_AFTER_SEC + 1
        snapshot = mqtt.snapshot()
        self.assertEqual(snapshot.online, {"tb3_1": False})
        self.assertTrue(snapshot.telemetry["tb3_1"]["stale"])
        self.assertTrue(mqtt._online["tb3_1"], "the raw connection state is untouched, only what QML sees changes")

        # Fresh telemetry brings it back.
        send("state", {})
        snapshot = mqtt.snapshot()
        self.assertEqual(snapshot.online, {"tb3_1": True})
        self.assertFalse(snapshot.telemetry["tb3_1"]["stale"])

    def test_control_queues_endpoint_changes_for_the_ros_thread(self):
        control = RosControl(config(identity("tb3_1", "0001")))
        control.add_robot(identity("tb3_2", "0002"))
        control.add_robot(identity("tb3_2", "0002"))
        self.assertEqual(control._speed_limits.keys(), {"tb3_1", "tb3_2"})
        self.assertEqual(control._command_queue.get_nowait()[:2], ("add_robot", "tb3_2"))
        self.assertTrue(control._command_queue.empty(), "a duplicate add is ignored")

        control.remove_robot("tb3_2")
        self.assertEqual(control._command_queue.get_nowait()[:2], ("remove_robot", "tb3_2"))
        self.assertEqual(list(control._speed_limits), ["tb3_1"])


if __name__ == "__main__":
    unittest.main()
