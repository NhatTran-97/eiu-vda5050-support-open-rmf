import unittest

from eiu_fleet_ui.config import RobotIdentity
from eiu_fleet_ui.registry_model import RegistryModel


def identity(name, manufacturer, serial, fleet="tb3_fleet"):
    return RobotIdentity(name=name, manufacturer=manufacturer, serial=serial, interface_name="AMR",
                         adapter_node="adapter", fleet_name=fleet)


def robot(name, manufacturer, serial, charger, source="config", retired=False):
    return {"name": name, "manufacturer": manufacturer, "serial": serial, "charger": charger,
            "source": source, "retired": retired}


def registry(fleet, robots, series="Burger", chargers=None, node=None):
    used = {r["charger"]: r["name"] for r in robots}
    names = chargers if chargers is not None else ["charger_1", "charger_2"]
    return {"fleet": fleet, "interface": "AMR", "adapter_node": node or f"adapter_{fleet}", "series": series,
            "limits": {}, "robots": robots,
            "chargers": [{"name": c, "used_by": used.get(c), "used_by_removed": False} for c in names]}


def snapshot(reporter, *robots):
    return {"reporter": reporter, "interface": "AMR", "robots": list(robots)}


def unknown(manufacturer, serial, series="Burger"):
    return {"manufacturer": manufacturer, "serial": serial, "series": series, "kinematic": "DIFF",
            "speed_max": 0.22, "pose": {"x": 1.0, "y": 2.0, "theta": 0.0, "map": "m", "initialized": True}}


class FollowingRobots(unittest.TestCase):
    def setUp(self):
        self.model = RegistryModel([identity("tb3_1", "ROBOTIS", "0001")])

    def test_runtime_robot_in_a_registry_is_followed_with_its_fleet_details(self):
        change = self.model.apply_registry(registry("tb3_fleet", [
            robot("tb3_1", "ROBOTIS", "0001", "charger_2"),
            robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")]))

        self.assertEqual([r.name for r in change.added], ["tb3_2"])
        added = change.added[0]
        self.assertEqual((added.fleet_name, added.interface_name, added.adapter_node, added.serial),
                         ("tb3_fleet", "AMR", "adapter_tb3_fleet", "0002"))
        self.assertEqual([r.name for r in self.model.followed()], ["tb3_1", "tb3_2"])

    def test_the_same_registry_twice_changes_nothing(self):
        message = registry("tb3_fleet", [robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")])
        self.model.apply_registry(message)
        again = self.model.apply_registry(message)
        self.assertEqual((again.added, again.removed), ([], []))

    def test_configured_robots_stay_even_if_a_registry_does_not_list_them(self):
        change = self.model.apply_registry(registry("tb3_fleet", []))
        self.assertEqual(change.removed, [])
        self.assertEqual([r.name for r in self.model.followed()], ["tb3_1"])

    def test_a_removed_robot_is_no_longer_followed(self):
        self.model.apply_registry(registry("tb3_fleet", [robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")]))
        change = self.model.apply_registry(registry("tb3_fleet", [
            robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime", retired=True)]))
        self.assertEqual(change.removed, ["tb3_2"])
        self.assertEqual([r.name for r in self.model.followed()], ["tb3_1"])

    def test_a_robot_a_restarted_fleet_no_longer_lists_is_dropped(self):
        self.model.apply_registry(registry("tb3_fleet", [robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")]))
        change = self.model.apply_registry(registry("tb3_fleet", []))
        self.assertEqual(change.removed, ["tb3_2"])

    def test_another_fleets_registry_does_not_drop_a_robot(self):
        self.model.apply_registry(registry("tb3_fleet", [robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")]))
        change = self.model.apply_registry(registry("amr_fleet", [robot("amr_1", "EIU-FABLAB", "0001", "charger_1")]))
        self.assertEqual(change.removed, [])
        self.assertEqual([r.name for r in change.added], ["amr_1"])

    def test_malformed_messages_are_ignored(self):
        for bad in (None, 5, "x", {}, {"fleet": 3, "robots": []}, {"fleet": "f", "robots": "no"}):
            change = self.model.apply_registry(bad)
            self.assertEqual((change.added, change.removed), ([], []))
        self.model.apply_discovery({"reporter": "f", "robots": "no"})
        self.model.apply_discovery(None)
        self.assertEqual(self.model.pending(), [])
        # Entries of a registry that are not objects are skipped.
        message = registry("tb3_fleet", [robot("tb3_2", "ROBOTIS", "0002", "c", "runtime")])
        message["robots"] = ["junk", 3, None] + message["robots"]
        change = self.model.apply_registry(message)
        self.assertEqual([r.name for r in change.added], ["tb3_2"])

    def test_source_comes_from_the_registry(self):
        self.model.apply_registry(registry("tb3_fleet", [
            robot("tb3_1", "ROBOTIS", "0001", "charger_2"),
            robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")]))
        self.assertEqual(self.model.source_of("tb3_1"), "config")
        self.assertEqual(self.model.source_of("tb3_2"), "runtime")
        self.assertEqual(self.model.source_of("ghost"), "")


class PendingRobots(unittest.TestCase):
    def setUp(self):
        self.model = RegistryModel([identity("tb3_1", "ROBOTIS", "0001")])
        self.model.apply_registry(registry("tb3_fleet", [robot("tb3_1", "ROBOTIS", "0001", "charger_2")]))
        self.model.apply_registry(registry("amr_fleet", [robot("amr_1", "EIU-FABLAB", "0001", "charger_1")], series="AMR"))

    def test_one_entry_per_robot_with_every_fleet_that_saw_it(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0002")))
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0002")))
        pending = self.model.pending()
        self.assertEqual([p["key"] for p in pending], ["ROBOTIS/0002"])
        self.assertEqual(sorted(pending[0]["reporters"]), ["amr_fleet", "tb3_fleet"])
        self.assertEqual(pending[0]["pose"]["map"], "m")

    def test_registered_robots_are_not_offered(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0001"), unknown("EIU-FABLAB", "0001"),
                                            unknown("ROBOTIS", "0002")))
        self.assertEqual([p["key"] for p in self.model.pending()], ["ROBOTIS/0002"])

    def test_a_removed_robot_that_is_online_again_is_offered_with_what_it_was(self):
        self.model.apply_registry(registry("tb3_fleet", [
            robot("tb3_1", "ROBOTIS", "0001", "charger_2"),
            robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime", retired=True)]))
        removed = dict(unknown("ROBOTIS", "0002", series=""), pose=None,
                       removed_as={"fleet": "tb3_fleet", "name": "tb3_2", "charger": "charger_1"})
        self.model.apply_discovery(snapshot("tb3_fleet", removed))

        [entry] = self.model.pending()
        self.assertEqual(entry["removed_as"], {"fleet": "tb3_fleet", "name": "tb3_2", "charger": "charger_1"})
        self.assertEqual(entry["suggested_fleet"], "tb3_fleet")
        self.assertEqual(entry["series"], "Burger", "the type comes from the fleet it belonged to")
        self.assertIn("tb3_fleet", entry["matching_fleets"])
        self.assertEqual(self.model.suggest_name("tb3_fleet", "ROBOTIS", "0002"), "tb3_2")
        self.assertEqual(self.model.suggest_charger("tb3_fleet", "ROBOTIS", "0002"), "charger_1")

    def test_a_malformed_removed_marker_is_ignored(self):
        broken = dict(unknown("ROBOTIS", "0002"), removed_as={"fleet": "tb3_fleet", "name": 7})
        self.model.apply_discovery(snapshot("tb3_fleet", broken))
        [entry] = self.model.pending()
        self.assertIsNone(entry["removed_as"])
        self.assertEqual(self.model.suggest_name("tb3_fleet", "ROBOTIS", "0002"), "tb3_2")

    def test_a_robot_that_was_never_removed_gets_the_usual_suggestions(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0003")))
        [entry] = self.model.pending()
        self.assertIsNone(entry["removed_as"])
        self.assertEqual(self.model.suggest_charger("tb3_fleet", "ROBOTIS", "0003"), "charger_1")

    def test_a_late_snapshot_of_another_fleet_cannot_resurrect_a_registered_robot(self):
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0002")))
        self.model.apply_registry(registry("tb3_fleet", [
            robot("tb3_1", "ROBOTIS", "0001", "charger_2"),
            robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")]))
        self.assertEqual(self.model.pending(), [])

    def test_a_new_snapshot_replaces_the_fleets_earlier_one(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0002"), unknown("ROBOTIS", "0003")))
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0003")))
        self.assertEqual([p["key"] for p in self.model.pending()], ["ROBOTIS/0003"])

    def test_dismissed_robots_are_hidden(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0002")))
        self.model.dismiss("ROBOTIS/0002")
        self.assertEqual(self.model.pending(), [])

    def test_a_robot_is_announced_once_and_again_after_it_leaves(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0002")))
        self.assertEqual(self.model.new_pending_keys(), ["ROBOTIS/0002"])
        self.assertEqual(self.model.new_pending_keys(), [])
        self.model.apply_discovery(snapshot("tb3_fleet"))
        self.assertEqual(self.model.new_pending_keys(), [])
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0002")))
        self.assertEqual(self.model.new_pending_keys(), ["ROBOTIS/0002"])


class Suggestions(unittest.TestCase):
    def setUp(self):
        self.model = RegistryModel()
        self.model.apply_registry(registry("tb3_fleet", [
            robot("tb3_1", "ROBOTIS", "0001", "charger_2"),
            robot("tb3_2", "ROBOTIS", "0002", "charger_1", source="runtime")], series="Burger"))
        self.model.apply_registry(registry("amr_fleet", [robot("amr_1", "EIU-FABLAB", "0001", "charger_1")],
                                           series="AMR", chargers=["charger_1", "charger_2", "dock_9"]))

    def test_the_fleet_of_the_robots_type_is_suggested(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0003", series="Burger")))
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0003", series="Burger")))
        self.assertEqual(self.model.pending()[0]["suggested_fleet"], "tb3_fleet")

    def test_no_fleet_is_suggested_when_the_type_is_unknown_or_unmatched(self):
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("A", "1", series=""), unknown("B", "2", series="Other")))
        self.assertEqual([p["suggested_fleet"] for p in self.model.pending()], ["", ""])

    def test_no_fleet_is_suggested_when_two_fleets_have_that_type(self):
        self.model.apply_registry(registry("tb3_fleet_b", [robot("b_1", "ROBOTIS", "0009", "charger_1")], series="Burger"))
        self.model.apply_discovery(snapshot("tb3_fleet", unknown("ROBOTIS", "0003")))
        self.model.apply_discovery(snapshot("tb3_fleet_b", unknown("ROBOTIS", "0003")))
        self.assertEqual(self.model.pending()[0]["suggested_fleet"], "")

    def test_fleets_of_the_robots_type_are_listed_even_when_they_do_not_see_it(self):
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0003", series="Burger")))
        entry = self.model.pending()[0]
        self.assertEqual((entry["reporters"], entry["matching_fleets"]), (["amr_fleet"], ["tb3_fleet"]))
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0003", series="Unknown type")))
        self.assertEqual(self.model.pending()[0]["matching_fleets"], [])
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0003", series="")))
        self.assertEqual(self.model.pending()[0]["matching_fleets"], [])

    def test_a_fleet_that_did_not_see_the_robot_is_not_suggested(self):
        self.model.apply_discovery(snapshot("amr_fleet", unknown("ROBOTIS", "0003", series="Burger")))
        self.assertEqual(self.model.pending()[0]["suggested_fleet"], "")

    def test_names_continue_the_fleets_numbering(self):
        self.assertEqual(self.model.suggest_name("tb3_fleet", "ROBOTIS", "0003"), "tb3_3")
        self.assertEqual(self.model.suggest_name("amr_fleet", "EIU-FABLAB", "0002"), "amr_2")

    def test_numbering_keeps_zero_padding_and_skips_names_taken_elsewhere(self):
        self.model.apply_registry(registry("pad_fleet", [robot("agv_007", "X", "7", "c"), robot("agv_009", "X", "9", "d")]))
        self.model.apply_registry(registry("other", [robot("agv_012", "Y", "1", "c")]))
        self.assertEqual(self.model.suggest_name("pad_fleet", "X", "10"), "agv_013")

    def test_names_come_from_the_identity_when_the_fleet_has_no_numbering(self):
        self.model.apply_registry(registry("plain", [robot("forklift", "ACME", "F1", "c")]))
        self.assertEqual(self.model.suggest_name("plain", "ACME", "F2"), "ACME_F2")
        self.assertEqual(self.model.suggest_name("nowhere", "we ird/maker", "s#1"), "we_ird_maker_s_1")
        self.assertEqual(self.model.suggest_name("nowhere", "___", "---"), "robot")

    def test_identity_based_names_are_made_unique(self):
        self.model.apply_registry(registry("plain", [robot("forklift", "ACME", "F1", "c")]))
        self.model.apply_registry(registry("other", [robot("ACME_F2", "ACME", "F9", "d")]))
        self.assertEqual(self.model.suggest_name("plain", "ACME", "F2"), "ACME_F2_2")

    def test_every_suggested_name_is_one_the_adapter_accepts(self):
        import re
        for maker, serial in (("we ird", "s#1"), ("é", "ü"), ("a" * 100, "b" * 100)):
            name = self.model.suggest_name("nowhere", maker, serial)
            self.assertRegex(name, r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")

    def test_the_first_free_charger_is_suggested(self):
        self.assertEqual(self.model.suggest_charger("tb3_fleet"), "")
        self.assertEqual(self.model.suggest_charger("amr_fleet"), "charger_2")
        self.assertEqual(self.model.suggest_charger("nowhere"), "")


if __name__ == "__main__":
    unittest.main()
