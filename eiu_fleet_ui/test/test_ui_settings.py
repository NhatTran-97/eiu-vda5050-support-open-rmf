import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

from eiu_fleet_ui import ui_settings


class UiSettingsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "ui_settings.yaml"
        self.env = mock.patch.dict(os.environ, {"EIU_UI_CONFIG": str(self.path)})
        self.env.start()
        for spec in ui_settings.SETTINGS.values():
            if spec.env:
                os.environ.pop(spec.env, None)
        ui_settings.reload()

    def tearDown(self):
        self.env.stop()
        ui_settings.reload()
        self.dir.cleanup()

    def read(self, text, key):
        self.path.write_text(text)
        ui_settings.reload()
        messages = io.StringIO()
        with redirect_stderr(messages):
            value = ui_settings.get(key)
        return value, messages.getvalue()

    def test_the_file_sets_values_and_the_rest_keep_their_defaults(self):
        value, messages = self.read("rmf:\n  offline_after_s: 8\n", "rmf.offline_after_s")
        self.assertEqual((value, messages), (8.0, ""))
        self.assertEqual(ui_settings.get("tasks.history"), 50)
        self.assertEqual(ui_settings.get("rmf.default_level"), "L1")

    def test_the_environment_overrides_the_file(self):
        self.path.write_text("tasks:\n  history: 30\n")
        with mock.patch.dict(os.environ, {"EIU_TASK_HISTORY": "12"}):
            ui_settings.reload()
            self.assertEqual(ui_settings.get("tasks.history"), 12)
        with mock.patch.dict(os.environ, {"EIU_TASK_HISTORY": "lots"}), redirect_stderr(io.StringIO()):
            self.assertEqual(ui_settings.get("tasks.history"), 30, "a bad variable falls back to the file")

    def test_bad_values_fall_back_to_the_default_and_say_so(self):
        cases = [("tasks:\n  history: 2.5\n", "tasks.history", 50),
                 ("rmf:\n  offline_after_s: -3\n", "rmf.offline_after_s", 5.0),
                 ("map:\n  min_zoom: yes\n", "map.min_zoom", 0.4),
                 ("rmf:\n  default_level: ''\n", "rmf.default_level", "L1"),
                 ("operator:\n  low_battery_percent: 180\n", "operator.low_battery_percent", 20.0)]
        for text, key, default in cases:
            value, messages = self.read(text, key)
            self.assertEqual(value, default, text)
            self.assertIn(key, messages, text)

    def test_unknown_keys_and_unreadable_files_are_reported(self):
        _, messages = self.read("rmf:\n  ofline_after_s: 3\n", "rmf.offline_after_s")
        self.assertIn("unknown setting 'rmf.ofline_after_s'", messages)
        value, messages = self.read("rmf: [1, 2\n", "rmf.offline_after_s")
        self.assertEqual(value, 5.0)
        self.assertIn("cannot read", messages)
        self.path.unlink()
        ui_settings.reload()
        with redirect_stderr(io.StringIO()) as messages:
            self.assertEqual(ui_settings.get("commands.timeout_s"), 10.0)
        self.assertIn("is not a file", messages.getvalue())

    def test_the_shipped_file_matches_the_defaults(self):
        shipped = Path(__file__).resolve().parents[1] / "config" / ui_settings.CONFIG_FILE
        with mock.patch.dict(os.environ, {"EIU_UI_CONFIG": str(shipped)}), redirect_stderr(io.StringIO()) as messages:
            ui_settings.reload()
            values = {key: ui_settings.get(key) for key in ui_settings.SETTINGS}
        self.assertEqual(messages.getvalue(), "")
        self.assertEqual(values, {key: spec.default for key, spec in ui_settings.SETTINGS.items()})


if __name__ == "__main__":
    unittest.main()
