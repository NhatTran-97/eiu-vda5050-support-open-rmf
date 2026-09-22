import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtNetwork import QHostAddress

from eiu_fleet_ui.config import FleetConfig, FleetSettings, RobotIdentity, _load_one_fleet, _merge_fleets, env_float
from eiu_fleet_ui.file_io import DebouncedWriter, user_config_dir, write_atomic
from eiu_fleet_ui.registry_model import RegistryModel
from eiu_fleet_ui.task_websocket import bind_address


class FileHelpers(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "sub" / "data.json"

    def tearDown(self):
        self.dir.cleanup()

    def test_atomic_write_creates_the_file_and_leaves_no_temporary(self):
        write_atomic(self.path, "one")
        write_atomic(self.path, "two")
        self.assertEqual(self.path.read_text(), "two")
        self.assertEqual([p.name for p in self.path.parent.iterdir()], ["data.json"])

    def test_a_new_file_gets_the_umask_mode_and_a_replaced_one_keeps_its_own(self):
        from eiu_fleet_ui import file_io
        write_atomic(self.path, "one")
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o666 & ~file_io._UMASK)
        self.path.chmod(0o640)
        write_atomic(self.path, "two")
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o640)

    def test_a_failed_write_keeps_the_old_content(self):
        write_atomic(self.path, "old")
        with mock.patch("eiu_fleet_ui.file_io.os.replace", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                write_atomic(self.path, "new")
        self.assertEqual(self.path.read_text(), "old")
        self.assertEqual([p.name for p in self.path.parent.iterdir()], ["data.json"])

    def test_the_debounced_writer_keeps_only_the_latest_text(self):
        writer = DebouncedWriter(self.path, 0.05)
        for text in ("a", "b", "c"):
            writer.submit(text)
        deadline = time.monotonic() + 3
        while not self.path.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        time.sleep(0.1)
        self.assertEqual(self.path.read_text(), "c")
        writer.submit("d")
        writer.close()
        self.assertEqual(self.path.read_text(), "d", "closing writes what is queued")

    def test_the_state_folder_follows_the_environment(self):
        with mock.patch.dict(os.environ, {"EIU_CONFIG_DIR": "/x/eiu"}):
            self.assertEqual(user_config_dir(), Path("/x/eiu"))
        with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": "/x/xdg"}, clear=True):
            self.assertEqual(user_config_dir(), Path("/x/xdg/eiu_fleet_ui"))


class Settings(unittest.TestCase):
    def test_numbers_come_from_the_environment_when_they_are_valid(self):
        with mock.patch.dict(os.environ, {"EIU_X": "2.5"}):
            self.assertEqual(env_float("EIU_X", 9.0), 2.5)
        for bad in ("", "abc", "0", "-1", "nan", "inf"):
            with mock.patch.dict(os.environ, {"EIU_X": bad}):
                self.assertEqual(env_float("EIU_X", 9.0), 9.0, bad)

    def test_the_websocket_listens_where_the_uri_points(self):
        loopback = QHostAddress(QHostAddress.SpecialAddress.LocalHost)
        self.assertEqual(bind_address("ws://localhost:9000", ""), loopback)
        self.assertEqual(bind_address("ws://192.168.1.20:9000", "").toString(), "192.168.1.20")
        self.assertEqual(bind_address("ws://ui-host:9000", ""), QHostAddress(QHostAddress.SpecialAddress.Any))
        self.assertEqual(bind_address("ws://localhost:9000", "any"), QHostAddress(QHostAddress.SpecialAddress.Any))
        self.assertEqual(bind_address("ws://localhost:9000", "10.0.0.5").toString(), "10.0.0.5")


def fleet(name, *robots):
    return FleetConfig(fleet_name=name, fleet_names=(name,), interface_name="AMR", broker_host="h", broker_port=1,
                       username=None, password=None,
                       robots=tuple(RobotIdentity(n, "M", s, "AMR", "node", name) for n, s in robots),
                       task_categories=("patrol",), nav_graph=None, websocket_uri=None, source="test")


def fleet_on(name, host, port):
    base = fleet(name)
    return FleetConfig(**{**base.__dict__, "broker_host": host, "broker_port": port})


class FleetsOnDifferentBrokers(unittest.TestCase):
    def test_fleets_on_one_broker_raise_nothing(self):
        merged = _merge_fleets([fleet_on("a", "h", 1883), fleet_on("b", "h", 1883)])
        self.assertEqual(merged.broker_conflicts, ())

    def test_fleets_on_different_brokers_are_listed_and_the_first_is_followed(self):
        merged = _merge_fleets([fleet_on("tb3", "192.168.10.97", 1883), fleet_on("amr", "192.168.1.121", 1883)])
        self.assertEqual(merged.broker_conflicts, (("tb3", "192.168.10.97", 1883), ("amr", "192.168.1.121", 1883)))
        self.assertEqual((merged.broker_host, merged.broker_port), ("192.168.10.97", 1883))

    def test_a_different_port_counts_as_a_different_broker(self):
        merged = _merge_fleets([fleet_on("a", "h", 1883), fleet_on("b", "h", 8883)])
        self.assertEqual(len(merged.broker_conflicts), 2)

    def test_the_settings_offer_the_clash_to_qml(self):
        merged = _merge_fleets([fleet_on("tb3", "x", 1), fleet_on("amr", "y", 1)])
        listed = json.loads(FleetSettings(merged).brokerConflictsJson)
        self.assertEqual(listed, [{"fleet": "tb3", "host": "x", "port": 1}, {"fleet": "amr", "host": "y", "port": 1}])
        self.assertEqual(FleetSettings(_merge_fleets([fleet_on("a", "x", 1)])).brokerConflictsJson, "[]")


class NavGraphChoice(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        root = Path(self.dir.name)
        (root / "config").mkdir()
        (root / "maps").mkdir()
        self.config = root / "config" / "config_x.yaml"
        self.config.write_text("rmf_fleet: {name: f}\nvda5050: {interface_name: AMR, robots: {r: {manufacturer: M, serial: '1'}}}\n")
        self.beside = root / "maps" / "nav_graph.yaml"
        self.beside.write_text("levels: {}\n")
        self.other = root / "other_graph.yaml"
        self.other.write_text("levels: {}\n")

    def tearDown(self):
        self.dir.cleanup()

    def graph(self):
        return _load_one_fleet([(self.config, "node", "test")]).nav_graph

    def test_the_graph_beside_the_config_is_used_by_default(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("EIU_NAV_GRAPH", None)
            self.assertEqual(self.graph(), self.beside)

    def test_a_named_graph_wins_and_a_wrong_name_falls_back(self):
        with mock.patch.dict(os.environ, {"EIU_NAV_GRAPH": str(self.other)}):
            self.assertEqual(self.graph(), self.other)
        with mock.patch.dict(os.environ, {"EIU_NAV_GRAPH": str(self.other) + ".missing"}):
            self.assertEqual(self.graph(), self.beside)


class SameNameInTwoFleets(unittest.TestCase):
    def test_the_config_keeps_the_first_robot_of_a_name(self):
        merged = _merge_fleets([fleet("a", ("r1", "1")), fleet("b", ("r1", "2"), ("r2", "3"))])
        self.assertEqual([(r.name, r.fleet_name) for r in merged.robots], [("r1", "a"), ("r2", "b")])

    def test_the_registry_reports_the_clash_and_clears_it(self):
        model = RegistryModel([RobotIdentity("r1", "M", "1", "AMR", "node", "a")])
        listing = lambda fleet_name, serial: {"fleet": fleet_name, "robots": [
            {"name": "r1", "manufacturer": "M", "serial": serial, "charger": "c", "source": "config"}]}
        model.apply_registry(listing("a", "1"))
        self.assertEqual(model.conflicts(), [])
        model.apply_registry(listing("b", "2"))
        self.assertEqual(model.conflicts(), [{"name": "r1", "fleet": "b", "followed_fleet": "a"}])
        self.assertEqual([r.serial for r in model.followed()], ["1"])
        model.apply_registry({"fleet": "b", "robots": []})
        self.assertEqual(model.conflicts(), [])


if __name__ == "__main__":
    unittest.main()
