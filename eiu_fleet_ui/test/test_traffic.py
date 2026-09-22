import unittest

from eiu_fleet_ui.vda5050 import traffic


class InstantSummary(unittest.TestCase):
    def test_action_shows_its_blocking_type(self):
        msg = {"actions": [{"actionType": "cancelOrder", "blockingType": "HARD"}]}
        self.assertEqual(traffic.instant_summary(msg), "cancelOrder · HARD")

    def test_several_actions_are_listed_in_order(self):
        msg = {"actions": [{"actionType": "startPause", "blockingType": "NONE"},
                           {"actionType": "stateRequest", "blockingType": "SOFT"}]}
        self.assertEqual(traffic.instant_summary(msg), "startPause · NONE, stateRequest · SOFT")

    def test_action_without_blocking_type_shows_only_its_type(self):
        self.assertEqual(traffic.instant_summary({"actions": [{"actionType": "stopPause"}]}), "stopPause")

    def test_message_without_actions(self):
        self.assertEqual(traffic.instant_summary({}), "(no actions)")
        self.assertEqual(traffic.instant_summary({"actions": ["junk"]}), "(no actions)")

    def test_action_without_type_is_marked(self):
        self.assertEqual(traffic.instant_summary({"actions": [{"blockingType": "HARD"}]}), "? · HARD")


if __name__ == "__main__":
    unittest.main()
