import datetime
import unittest

from eiu_fleet_ui import task_state as ts


def task(state="queued", rank=0, **fields):
    record = {"id": "r1", "state": state, "state_rank": rank, "end_ms": None, "end_estimated": False}
    record.update(fields)
    return record


class SetStateTest(unittest.TestCase):
    def test_any_source_moves_a_task_forward(self):
        t = task()
        self.assertTrue(ts.set_state(t, "underway", "fleet"))
        self.assertTrue(ts.set_state(t, "completed", "fleet", at_ms=5000))
        self.assertEqual((t["state"], t["end_ms"], t["end_estimated"]), ("completed", 5000, False))

    def test_a_guess_is_corrected_by_a_better_informed_source(self):
        t = task("underway", ts.SOURCE_RANK["api"])
        ts.set_state(t, "completed", "fleet", at_ms=1)
        self.assertEqual(t["state"], "completed", "the fleet guess moves an active task forward")
        self.assertTrue(ts.set_state(t, "failed", "dispatch", at_ms=2))
        self.assertEqual(t["state"], "failed")
        self.assertEqual(t["end_ms"], 1, "the end time stays when it was already a real one")

    def test_a_lagging_source_cannot_move_a_task_back(self):
        t = task("underway", ts.SOURCE_RANK["events"])
        self.assertFalse(ts.set_state(t, "queued", "dispatch"))
        self.assertFalse(ts.set_state(t, "queued", "events"))
        self.assertEqual(t["state"], "underway")

    def test_a_finished_task_only_changes_for_a_better_source(self):
        t = task("completed", ts.SOURCE_RANK["api"], end_ms=10)
        self.assertFalse(ts.set_state(t, "failed", "dispatch"))
        self.assertFalse(ts.set_state(t, "failed", "api"), "same rank cannot change one ending for another")
        wrong_guess = task("completed", ts.SOURCE_RANK["fleet"], end_ms=10)
        self.assertTrue(ts.set_state(wrong_guess, "underway", "events"))
        self.assertEqual((wrong_guess["end_ms"], wrong_guess["end_estimated"]), (None, False))

    def test_the_same_state_from_a_better_source_raises_the_rank(self):
        t = task("underway", ts.SOURCE_RANK["fleet"])
        self.assertFalse(ts.set_state(t, "underway", "api"))
        self.assertEqual(t["state_rank"], ts.SOURCE_RANK["api"])
        self.assertFalse(ts.set_state(t, "queued", "dispatch"))

    def test_unknown_states_are_ignored(self):
        t = task()
        self.assertFalse(ts.set_state(t, "exploded", "api"))
        self.assertEqual(t["state"], "queued")

    def test_an_estimated_end_is_replaced_by_the_real_one(self):
        t = task("underway")
        self.assertTrue(ts.set_estimated_end(t, 60_000))
        self.assertFalse(ts.set_estimated_end(t, 60_500), "half a second is not a new estimate")
        self.assertTrue(ts.set_estimated_end(t, 90_000))
        ts.set_state(t, "completed", "api", at_ms=80_000)
        self.assertEqual((t["end_ms"], t["end_estimated"]), (80_000, False))
        self.assertFalse(ts.set_estimated_end(t, 99_000), "a finished task keeps its real end")


class TimesTest(unittest.TestCase):
    def test_epoch_ms_reads_seconds_and_milliseconds(self):
        self.assertEqual(ts.epoch_ms(1_700_000_000), 1_700_000_000_000)
        self.assertEqual(ts.epoch_ms(1_700_000_000_123), 1_700_000_000_123)
        self.assertEqual(ts.epoch_ms("1700000000.5"), 1_700_000_000_500)
        for missing in (None, 0, -5, "soon"):
            self.assertIsNone(ts.epoch_ms(missing))

    def test_new_tasks_start_queued_now(self):
        t = ts.new_task("r9", "eiu_fleet_ui", destination="wp", robot="tb3_1")
        self.assertEqual((t["state"], t["destination"], t["robot"], t["end_ms"]), ("queued", "wp", "tb3_1", None))
        self.assertAlmostEqual(t["created_ms"], ts.now_ms(), delta=2000)

    def test_old_cache_records_get_epoch_times_in_local_time(self):
        old = {"id": "r1", "state": "completed", "date": "23 Sep 2026", "start": "11:50:00 PM", "end": "12:10:30 AM"}
        migrated = ts.migrate(old)
        start = int(datetime.datetime(2026, 9, 23, 23, 50, 0).timestamp() * 1000)
        end = int(datetime.datetime(2026, 9, 24, 0, 10, 30).timestamp() * 1000)
        self.assertEqual((migrated["created_ms"], migrated["end_ms"], migrated["end_estimated"]), (start, end, False))
        self.assertNotIn("date", migrated)

    def test_unreadable_old_times_become_unknown(self):
        migrated = ts.migrate({"id": "r1", "state": "underway", "date": "—", "start": "—", "end": "—"})
        self.assertEqual((migrated["created_ms"], migrated["end_ms"]), (None, None))
        current = {"id": "r2", "created_ms": 5, "end_ms": None}
        self.assertIs(ts.migrate(current), current)


if __name__ == "__main__":
    unittest.main()
