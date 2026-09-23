import unittest

from eiu_fleet_ui import dashboard_model as dm


def rmf_row(name, fleet="fleet_a", x=1.0, y=2.0, status="MOVING", task="", path=None, battery=50.0):
    return {"key": f"{fleet}/{name}", "name": name, "fleet": fleet, "model": "tb3", "status": status,
            "battery": battery, "level": "L1", "task": task, "x": x, "y": y, "yaw": 0.0,
            "path": path or []}


def tele(**fields):
    base = {"speed": 0.3, "stale": False, "operating_mode": "AUTOMATIC", "position_initialized": True,
            "safety": {"e_stop": "NONE", "field_violation": False, "triggered": False}, "fatal_error": "",
            "charging": False, "paused": False, "battery_soc": None, "last_rx": 1000.0, "node_states": [],
            "off_graph": False, "off_graph_m": None, "map_id": "L1"}
    base.update(fields)
    return base


class DisplayRobotsTest(unittest.TestCase):
    def test_rmf_robots_and_placeholders_in_config_order(self):
        rows = dm.display_robots([("b", "fleet_a"), ("a", "fleet_a")], [rmf_row("a")], {}, {"a": True}, [], "fleet_a")
        self.assertEqual([r["name"] for r in rows], ["b", "a"])
        self.assertEqual(rows[0]["status"], dm.PENDING_SYNC)
        self.assertFalse(rows[0]["rmfSynced"])
        self.assertTrue(rows[1]["rmfSynced"])
        self.assertEqual((rows[0]["online"], rows[1]["online"]), (False, True))

    def test_vda5050_battery_replaces_the_planning_value(self):
        rows = dm.display_robots([("a", "f")], [rmf_row("a", battery=50.0)], {"a": tele(battery_soc=0.8)}, {}, [], "f")
        self.assertAlmostEqual(rows[0]["battery"], 80.0)
        placeholder = dm.display_robots([("b", "g")], [], {"b": tele(battery_soc=0.4, map_id="L2")}, {}, [], "f")[0]
        self.assertTrue(placeholder["hasBattery"])
        self.assertAlmostEqual(placeholder["battery"], 40.0)
        self.assertEqual((placeholder["fleet"], placeholder["level"], placeholder["key"]), ("g", "L2", "g/b"))

    def test_robots_of_a_silent_fleet_say_so(self):
        rows = dm.display_robots([("a", "fleet_a"), ("b", "fleet_b")], [rmf_row("a"), rmf_row("b", fleet="fleet_b")],
                                 {}, {"a": True, "b": True}, [], "fleet_a", stale_fleets={"fleet_b"})
        self.assertEqual([r["status"] for r in rows], ["MOVING", dm.NO_RMF_DATA])
        self.assertEqual(dm.fleet_status_summary(rows), "1 navigating · 1 no RMF data")

    def test_the_current_task_gives_destination_and_rounds(self):
        tasks = [{"rmf_id": "t1", "destination": "wp_3", "rounds": 3, "rounds_remaining": 2}]
        row = dm.display_robots([("a", "f")], [rmf_row("a", task="t1")], {}, {}, tasks, "f")[0]
        self.assertEqual(row["task_destination"], "wp_3")
        self.assertEqual((row["rounds_total"], row["rounds_remaining"], row["rounds_current"]), (3, 2, 2))
        single = dm.display_robots([("a", "f")], [rmf_row("a", task="t2")],
                                   {}, {}, [{"rmf_id": "t2", "destination": "x", "rounds": 1}], "f")[0]
        self.assertEqual((single["rounds_total"], single["rounds_current"]), (0, 0))

    def test_telemetry_summary_names_the_worst_safety_state(self):
        self.assertIsNone(dm.telemetry_summary(None))
        self.assertEqual(dm.telemetry_summary(tele(fatal_error="motor"))["safety_label"], "motor")
        e_stop = tele(safety={"e_stop": "MANUAL", "field_violation": True, "triggered": True})
        self.assertEqual(dm.telemetry_summary(e_stop)["safety_label"], "MANUAL")
        field = tele(safety={"e_stop": "NONE", "field_violation": True, "triggered": True})
        summary = dm.telemetry_summary(field)
        self.assertEqual((summary["safety_label"], summary["unsafe"]), ("FIELD VIOLATION", True))
        manual = dm.telemetry_summary(tele(operating_mode="MANUAL", position_initialized=False, stale=True))
        self.assertEqual((manual["manual"], manual["not_localized"], manual["stale"]), (True, True, True))

    def test_fleet_summary_counts_offline_before_status(self):
        rows = [{"status": "MOVING", "online": True}, {"status": "CHARGING", "online": True},
                {"status": "ERROR", "online": True}, {"status": "IDLE", "online": True},
                {"status": "MOVING", "online": False}]
        self.assertEqual(dm.fleet_status_summary(rows), "1 error · 1 navigating · 1 charging · 1 idle · 1 offline")
        self.assertEqual(dm.fleet_status_summary([]), "No robots")

    def test_robot_filter_matches_names(self):
        rows = [{"name": "tb3_1"}, {"name": "AMR_2"}]
        self.assertEqual(dm.filter_robots(rows, " amr "), [{"name": "AMR_2"}])
        self.assertIs(dm.filter_robots(rows, ""), rows)


class TasksTest(unittest.TestCase):
    TASKS = [
        {"id": "1", "state": "completed", "robot": "a", "destination": "wp_1", "requester": "ui"},
        {"id": "2", "state": "underway", "robot": "b", "destination": "wp_2", "requester": "ui"},
        {"id": "3", "state": "queued", "robot": "—", "destination": "dock", "requester": "api", "cancel": "failed"},
        {"id": "4", "state": "underway", "robot": "c", "destination": "wp_4", "requester": "ui", "cancel": "requested"},
    ]

    def test_underway_tasks_come_first_keeping_their_order(self):
        self.assertEqual([t["id"] for t in dm.filter_tasks(self.TASKS, "All", "")], ["2", "4", "1", "3"])

    def test_state_filter_and_search(self):
        self.assertEqual([t["id"] for t in dm.filter_tasks(self.TASKS, "Underway", "")], ["2", "4"])
        self.assertEqual([t["id"] for t in dm.filter_tasks(self.TASKS, "All", "API")], ["3"])
        self.assertEqual([t["id"] for t in dm.filter_tasks(self.TASKS, "Completed", "wp_")], ["1"])

    def test_rows_carry_the_display_state_and_whether_they_can_be_cancelled(self):
        rows = {r["id"]: r for r in dm.task_rows(self.TASKS)}
        self.assertEqual(rows["3"]["display_state"], "cancel failed")
        self.assertEqual(rows["4"]["display_state"], "cancelling")
        self.assertEqual({k: r["cancellable"] for k, r in rows.items()}, {"1": False, "2": True, "3": True, "4": False})

    def test_counts_and_destinations(self):
        self.assertEqual(dm.task_counts(self.TASKS), {"underway": 2, "queued": 1, "completed": 1, "failed": 0})
        self.assertEqual(dm.active_destinations(self.TASKS), {"wp_2": True, "wp_4": True})


class AttentionTest(unittest.TestCase):
    def items(self, **overrides):
        args = dict(rmf_online=True, mqtt_connected=True, active_conflicts=0, blocked_lanes=0, broker_clashes=[],
                    adapter_items=[], robots=[], telemetry={}, tasks=[], name_conflicts=[], pending=[])
        args.update(overrides)
        return dm.attention_items(**args)

    def test_nothing_to_report(self):
        self.assertEqual(self.items(), [])
        self.assertEqual(dm.health([], True, True), ("HEALTHY", "All services nominal"))

    def test_losing_every_robot_is_critical_and_one_is_a_warning(self):
        robots = [{"name": "a", "online": False, "battery": 50}, {"name": "b", "online": False, "battery": 50}]
        items = self.items(robots=robots, telemetry={"a": tele(last_rx=990.0)})
        self.assertEqual([(i["key"], i["severity"]) for i in items],
                         [("robot:a:offline", "critical"), ("robot:b:offline", "critical")])
        self.assertEqual((items[0]["since"], items[0]["detail"]), (990.0, "No VDA5050 state received"))
        self.assertEqual((items[1]["since"], items[1]["detail"]), (0.0, "No VDA5050 state ever received"))
        robots[1]["online"] = True
        self.assertEqual(self.items(robots=robots)[0]["severity"], "warning")

    def test_keys_stay_the_same_while_the_detail_changes(self):
        robots = [{"name": "a", "online": True, "battery": 12}]
        first = self.items(robots=robots, blocked_lanes=1)
        robots[0]["battery"] = 9
        second = self.items(robots=robots, blocked_lanes=2)
        self.assertEqual([i["key"] for i in first], [i["key"] for i in second])
        self.assertNotEqual(first, second)

    def test_robot_problems(self):
        robots = [{"name": "a", "online": True, "battery": 10}]
        telemetry = {"a": tele(safety={"e_stop": "AUTOACK", "field_violation": False, "triggered": True},
                                fatal_error="lidar", position_initialized=False, stale=True, off_graph=True,
                                off_graph_m=2.34, paused=True)}
        items = self.items(robots=robots, telemetry=telemetry)
        self.assertEqual([i["key"].split(":")[-1] for i in items],
                         ["estop", "fatal", "battery", "localization", "stale", "off_graph", "paused"])
        self.assertEqual(items[0]["detail"], "AUTOACK")
        self.assertIn("2.3 m", items[5]["detail"])
        self.assertTrue(all(i["robot"] == "a" for i in items))

    def test_low_battery_follows_the_limit(self):
        robots = [{"name": "a", "online": True, "battery": 25}]
        self.assertEqual(self.items(robots=robots), [])
        items = self.items(robots=robots, limits=dm.Limits(low_battery_percent=30))
        self.assertEqual([i["key"] for i in items], ["robot:a:battery"])

    def test_system_and_registration_items(self):
        pending = [{"key": "M/1", "manufacturer": "M", "serial": "1", "series": "", "removed_as": None},
                   {"key": "M/2", "manufacturer": "M", "serial": "2", "series": "x",
                    "removed_as": {"name": "r2", "fleet": "f"}}]
        items = self.items(rmf_online=False, mqtt_connected=False, active_conflicts=2, blocked_lanes=1,
                           broker_clashes=[{"fleet": "f", "host": "h", "port": 1}],
                           adapter_items=[{"key": "adapter:n:mqtt", "severity": "critical", "title": "t", "detail": "d"}],
                           tasks=[{"state": "failed"}], name_conflicts=[{"name": "r", "fleet": "g", "followed_fleet": "f"}],
                           pending=pending, stale_fleets=[{"fleet": "f", "last_rx": 5.0}])
        self.assertEqual([i["key"] for i in items],
                         ["rmf:offline", "rmf:stale:f", "mqtt:offline", "traffic:conflicts", "traffic:blocked",
                          "config:brokers", "adapter:n:mqtt", "tasks:failed", "config:name:r", "pending:M/1", "pending:M/2"])
        self.assertIs(items[-1]["pending"], pending[1])
        self.assertIn("Removed robot is online again: r2", items[-1]["title"])
        self.assertEqual(items[-2]["detail"], "Type unknown · not registered in any fleet")

    def test_health_levels(self):
        warn = [{"severity": "warning", "title": "w1"}]
        crit = [{"severity": "critical", "title": "c1"}, {"severity": "critical", "title": "c2"}]
        info = [{"severity": "info", "title": "i"}]
        self.assertEqual(dm.health(warn, True, True), ("DEGRADED", "w1"))
        self.assertEqual(dm.health(crit + warn, True, True), ("CRITICAL", "2 critical issues — see Needs Attention"))
        self.assertEqual(dm.health(info, True, True)[0], "HEALTHY")
        self.assertEqual(dm.health(crit, False, False), ("OFFLINE", "No connection to fleet"))


class MapTest(unittest.TestCase):
    WAYPOINTS = {"wp_1": {"x": 1.0, "y": 1.0}, "wp_2": {"x": 5.0, "y": 1.0}, "wp_3": {"x": 5.0, "y": 5.0}}

    def test_markers_carry_pose_status_and_link(self):
        markers = dm.map_robots([rmf_row("a", x=3.0, y=4.0)], {"a": True})
        self.assertEqual(markers, [{"key": "fleet_a/a", "name": "a", "fleet": "fleet_a", "status": "MOVING",
                                    "x": 3.0, "y": 4.0, "yaw": 0.0, "online": True}])

    def test_route_follows_the_order_nodes_and_heads_for_the_task_destination(self):
        row = rmf_row("a", x=0.5, y=1.0, path=[{"x": 5.0, "y": 1.0}, {"x": 5.0, "y": 5.0}])
        nodes = [{"nodeId": "wp_2", "released": True}, {"nodeId": "wp_3", "released": False},
                 {"nodeId": "unknown", "released": False}]
        route = dm.routes([row], {"a": tele(node_states=nodes)}, {"a": True}, self.WAYPOINTS, {"a": "wp_3"})[0]
        self.assertEqual(route["lines"], [{"style": "released", "points": [[0.5, 1.0], [5.0, 1.0]]},
                                          {"style": "planned", "points": [[5.0, 1.0], [5.0, 5.0]]}])
        self.assertEqual((route["dest"], route["tail"]), ({"x": 5.0, "y": 5.0}, {"x": 5.0, "y": 1.0}))

    def test_without_order_nodes_the_rmf_path_is_drawn(self):
        row = rmf_row("a", x=0.5, y=1.0, path=[{"x": 2.0, "y": 1.0}])
        route = dm.routes([row], {}, {"a": True}, self.WAYPOINTS, {})[0]
        self.assertEqual(route["lines"], [{"style": "released", "points": [[0.5, 1.0], [2.0, 1.0]]}])
        self.assertEqual((route["dest"], route["tail"]), ({"x": 2.0, "y": 1.0}, {"x": 0.5, "y": 1.0}))

    def test_offline_or_idle_robots_draw_nothing(self):
        moving = rmf_row("a", path=[{"x": 2.0, "y": 1.0}])
        self.assertEqual(dm.routes([moving], {}, {"a": False}, self.WAYPOINTS, {}), [])
        self.assertEqual(dm.routes([rmf_row("b")], {}, {"b": True}, self.WAYPOINTS, {}), [])
        parked = dm.routes([rmf_row("c")], {}, {"c": True}, self.WAYPOINTS, {"c": "wp_1"})
        self.assertEqual(parked, [{"key": "fleet_a/c", "lines": [], "dest": {"x": 1.0, "y": 1.0}, "tail": None}])

    def test_traffic_filter(self):
        entries = [{"id": 1, "type": "order"}, {"id": 2, "type": "state"}]
        self.assertEqual(dm.filter_traffic(entries, "state"), [{"id": 2, "type": "state"}])
        self.assertIs(dm.filter_traffic(entries, "all"), entries)


if __name__ == "__main__":
    unittest.main()
