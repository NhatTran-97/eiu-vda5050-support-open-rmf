import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QCoreApplication

from eiu_fleet_ui.list_model import KEY_ROLE, ROW_ROLE, KeyedListModel


def rows(*keys, **values):
    return [{"key": k, "v": values.get(k, 0)} for k in keys]


class Recorder:
    """Counts the change notifications a model sends."""

    def __init__(self, model):
        self.inserted = self.removed = self.moved = self.changed = self.counts = self.resets = 0
        model.rowsInserted.connect(lambda *_: self._bump("inserted"))
        model.rowsRemoved.connect(lambda *_: self._bump("removed"))
        model.rowsMoved.connect(lambda *_: self._bump("moved"))
        model.dataChanged.connect(lambda *_: self._bump("changed"))
        model.countChanged.connect(lambda: self._bump("counts"))
        model.modelReset.connect(lambda: self._bump("resets"))

    def _bump(self, name):
        setattr(self, name, getattr(self, name) + 1)


class KeyedListModelTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def keys(self, model):
        return [model.get(i)["key"] for i in range(model.count)]

    def test_the_same_rows_change_nothing(self):
        model = KeyedListModel()
        model.set_rows(rows("a", "b"))
        seen = Recorder(model)
        self.assertFalse(model.set_rows(rows("a", "b")))
        self.assertEqual((seen.inserted, seen.removed, seen.moved, seen.changed, seen.counts), (0, 0, 0, 0, 0))

    def test_a_changed_value_updates_only_its_row(self):
        model = KeyedListModel()
        model.set_rows(rows("a", "b", "c"))
        seen = Recorder(model)
        changed_rows = []
        model.dataChanged.connect(lambda top, bottom, roles: changed_rows.append((top.row(), list(roles))))
        self.assertTrue(model.set_rows(rows("a", "b", "c", b=5)))
        self.assertEqual(changed_rows, [(1, [ROW_ROLE])])
        self.assertEqual((seen.inserted, seen.removed, seen.moved), (0, 0, 0))
        self.assertEqual(model.get(1)["v"], 5)

    def test_rows_are_inserted_removed_and_moved_never_reset(self):
        model = KeyedListModel()
        model.set_rows(rows("a", "b", "c", "d"))
        seen = Recorder(model)
        model.set_rows(rows("d", "x", "b", "a"))
        self.assertEqual(self.keys(model), ["d", "x", "b", "a"])
        self.assertEqual((seen.inserted, seen.removed), (1, 1))
        self.assertGreaterEqual(seen.moved, 1)
        self.assertEqual(seen.resets, 0)
        self.assertEqual(seen.counts, 0, "four rows before and after")

    def test_count_follows_the_rows(self):
        model = KeyedListModel()
        seen = Recorder(model)
        model.set_rows(rows("a", "b"))
        model.set_rows(rows("a"))
        model.set_rows([])
        self.assertEqual(model.count, 0)
        self.assertEqual(seen.counts, 3)

    def test_a_repeated_key_keeps_its_first_row(self):
        model = KeyedListModel()
        model.set_rows([{"key": "a", "v": 1}, {"key": "a", "v": 2}, {"key": "b", "v": 3}])
        self.assertEqual(self.keys(model), ["a", "b"])
        self.assertEqual(model.get(0)["v"], 1)

    def test_rows_are_read_through_the_key_and_row_roles(self):
        model = KeyedListModel(key="id")
        model.set_rows([{"id": 7, "name": "task"}])
        index = model.index(0)
        self.assertEqual(model.data(index, KEY_ROLE), "7")
        self.assertEqual(model.data(index, ROW_ROLE), {"id": 7, "name": "task"})
        self.assertEqual(model.indexOf("7"), 0)
        self.assertEqual(model.indexOf("8"), -1)
        self.assertIsNone(model.get(3))
        self.assertEqual({bytes(name) for name in model.roleNames().values()}, {b"key", b"row"})

    def test_qml_can_set_rows_and_other_entries_are_skipped(self):
        model = KeyedListModel()
        model.setRows([{"key": "a"}, "not a row", {"key": "b"}])
        self.assertEqual(self.keys(model), ["a", "b"])


if __name__ == "__main__":
    unittest.main()
