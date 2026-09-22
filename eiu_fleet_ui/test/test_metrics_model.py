import unittest

from eiu_fleet_ui.metrics_model import ABSENT, CRITICAL, FOUND, OK, SILENT, WAITING, WARNING, MetricsModel


def report(states=0, dropped=0, overruns=0, failed=0, lost=0, connected=True, interval=60.0, online=2, registered=2,
           age=0.5, handle_p99=90.0, loop_p99=200.0, fleet="tb3_fleet"):
    """A report shaped like the adapter's, with the counts that grow from its start."""
    return {
        "fleet": fleet, "uptime_s": 100.0, "interval_s": interval,
        "robots": {"registered": registered, "online": online, "without_state": 0, "state_timeout_s": 10.0,
                   "state_age_max_s": age, "state_age_mean_s": age, "oldest_state_robot": "tb3_1"},
        "rx": {"state": states, "visualization": states * 2, "connection": 1, "factsheet": 0, "unregistered": 5},
        "dropped": {"bad_payload": dropped, "bad_topic": 0, "invalid_state": 0, "stale_state": 0, "oversize": 0},
        "log_suppressed": 0,
        "published": {"ok": 10, "failed": failed},
        "mqtt": {"connected": connected, "connects": 1, "connections_lost": lost, "errors": 0},
        "latency_us": {"handle_state": {"count": 20, "mean": 50.0, "p50": 45.0, "p99": handle_p99, "max": 120.0},
                       "handle_other": {"count": 0, "mean": 0, "p50": 0, "p99": 0, "max": 0},
                       "mutex_wait": {"count": 20, "mean": 0.1, "p50": 0.2, "p99": 1.0, "max": 3.0}},
        "state_transit_us": {"count": 0, "mean": 0, "p50": 0, "p99": 0, "max": 0},
        "update_loop": {"pass_us": {"count": 600, "mean": 100.0, "p50": 90.0, "p99": loop_p99, "max": 900.0},
                        "overruns": overruns, "period_ms": 100.0},
    }


class Series(unittest.TestCase):
    def setUp(self):
        self.model = MetricsModel(capacity=4)

    def test_the_rate_comes_from_the_growth_of_the_counts_over_the_interval(self):
        self.model.apply("node", report(states=100), 1000.0)
        self.model.apply("node", report(states=220, interval=60.0), 1060.0)
        series = self.model.snapshot(1060.0)["adapters"][0]["series"]
        self.assertIsNone(series["msg_per_s"][0])            # nothing to compare the first report with
        self.assertAlmostEqual(series["msg_per_s"][1], 120 * 3 / 60.0)   # states + twice as many visualizations

    def test_a_restarted_adapter_counts_from_zero(self):
        self.model.apply("node", report(states=1000), 1000.0)
        self.model.apply("node", report(states=30), 1060.0)
        self.assertAlmostEqual(self.model.snapshot(1060.0)["adapters"][0]["series"]["msg_per_s"][1], (30 * 3 + 1) / 60.0)

    def test_only_the_newest_samples_are_kept(self):
        for i in range(10):
            self.model.apply("node", report(states=i), 1000.0 + i * 60)
        self.assertEqual(len(self.model.snapshot(1600.0)["adapters"][0]["series"]["age_s"]), 4)

    def test_the_series_carry_the_latest_figures_of_each_report(self):
        self.model.apply("node", report(handle_p99=123.0, loop_p99=2500.0, age=1.5), 1000.0)
        series = self.model.snapshot(1030.0)["adapters"][0]["series"]
        self.assertEqual(series["handle_p99_us"], [123.0])
        self.assertEqual(series["loop_p99_ms"], [2.5])
        self.assertEqual(series["state_age_max_s"], [1.5])
        self.assertEqual(series["age_s"], [30.0])

    def test_an_interval_without_messages_leaves_a_gap_not_a_zero(self):
        quiet = report()
        quiet["latency_us"]["handle_state"]["count"] = 0
        quiet["update_loop"]["pass_us"]["count"] = 0
        self.model.apply("node", quiet, 1000.0)
        series = self.model.snapshot(1000.0)["adapters"][0]["series"]
        self.assertEqual(series["handle_p99_us"], [None])
        self.assertEqual(series["loop_p99_ms"], [None])


class Snapshot(unittest.TestCase):
    def test_it_describes_the_adapter_from_its_latest_report(self):
        model = MetricsModel()
        model.apply("adapter_tb3", report(states=10, online=1, registered=2, age=2.0), 1000.0)
        adapter = model.snapshot(1005.0)["adapters"][0]
        self.assertEqual(adapter["node"], "adapter_tb3")
        self.assertEqual(adapter["fleet"], "tb3_fleet")
        self.assertEqual(adapter["status"], OK)
        self.assertEqual(adapter["reported_ago_s"], 5.0)
        self.assertEqual(adapter["robots"]["online"], 1)
        self.assertEqual(adapter["robots"]["registered"], 2)
        self.assertEqual(adapter["robots"]["state_timeout_s"], 10.0)
        self.assertEqual(adapter["robots"]["oldest_state_robot"], "tb3_1")
        self.assertEqual(adapter["period_ms"], 100.0)
        self.assertEqual(adapter["latency_us"]["handle_state"]["p99"], 90.0)
        self.assertEqual(adapter["totals"]["unregistered"], 5)
        self.assertTrue(adapter["mqtt"]["connected"])

    def test_an_expected_adapter_that_has_not_reported_is_waiting(self):
        model = MetricsModel()
        model.expect("adapter_amr", "amr_fleet")
        adapter = model.snapshot(1000.0)["adapters"][0]
        self.assertEqual(adapter["status"], WAITING)
        self.assertEqual(adapter["fleet"], "amr_fleet")
        self.assertIsNone(adapter["reported_ago_s"])
        self.assertEqual(model.attention(1000.0), [])

    def test_a_report_that_is_not_an_object_is_ignored(self):
        model = MetricsModel()
        for junk in (None, [], "text", 5):
            model.apply("node", junk, 1000.0)
        self.assertEqual(model.nodes(), [])

    def test_missing_or_mistyped_fields_read_as_zero(self):
        model = MetricsModel()
        model.apply("node", {"interval_s": 60, "robots": "none", "rx": {"state": "many"}, "mqtt": []}, 1000.0)
        adapter = model.snapshot(1000.0)["adapters"][0]
        self.assertEqual(adapter["robots"]["registered"], 0)
        self.assertEqual(adapter["totals"]["rx"], 0)

    def test_nodes_are_followed_in_the_order_they_were_first_seen(self):
        model = MetricsModel()
        model.expect("b")
        model.apply("a", report(), 1000.0)
        model.expect("b")
        self.assertEqual(model.nodes(), ["b", "a"])


class Attention(unittest.TestCase):
    def setUp(self):
        self.model = MetricsModel()

    def apply_pair(self, first, second, gap=60.0):
        self.model.apply("node", first, 1000.0)
        self.model.apply("node", second, 1000.0 + gap)
        return 1000.0 + gap

    def test_a_healthy_adapter_raises_nothing(self):
        now = self.apply_pair(report(states=10), report(states=20))
        self.assertEqual(self.model.attention(now), [])
        self.assertEqual(self.model.snapshot(now)["adapters"][0]["status"], OK)

    def test_a_lost_broker_is_critical(self):
        now = self.apply_pair(report(), report(connected=False, lost=1))
        items = self.model.attention(now)
        self.assertEqual([i["severity"] for i in items], ["critical"])
        self.assertIn("MQTT", items[0]["title"])
        self.assertEqual(self.model.snapshot(now)["adapters"][0]["status"], CRITICAL)

    def test_dropped_messages_in_the_last_interval_are_a_warning_that_names_them(self):
        now = self.apply_pair(report(dropped=2), report(dropped=5))
        items = self.model.attention(now)
        self.assertEqual(items[0]["severity"], "warning")
        self.assertIn("dropped 3 message(s)", items[0]["title"])
        self.assertIn("bad payload 3", items[0]["detail"])
        self.assertEqual(self.model.snapshot(now)["adapters"][0]["status"], WARNING)

    def test_the_warning_clears_once_a_report_shows_no_new_drops(self):
        self.model.apply("node", report(dropped=2), 1000.0)
        self.model.apply("node", report(dropped=5), 1060.0)
        self.model.apply("node", report(dropped=5), 1120.0)
        self.assertEqual(self.model.attention(1120.0), [])

    def test_the_first_report_only_sets_the_baseline(self):
        self.model.apply("node", report(dropped=40, overruns=7, failed=3), 1000.0)
        self.assertEqual(self.model.attention(1000.0), [])

    def test_an_update_loop_that_overran_is_reported_with_the_period(self):
        now = self.apply_pair(report(), report(overruns=2))
        item = self.model.attention(now)[0]
        self.assertIn("fell behind", item["title"])
        self.assertIn("2 pass(es) over the 100 ms period", item["detail"])

    def test_refused_publishes_are_reported(self):
        now = self.apply_pair(report(), report(failed=4))
        self.assertIn("could not send 4 message(s)", self.model.attention(now)[0]["title"])

    def test_an_adapter_that_stopped_reporting_is_silent_after_three_intervals(self):
        self.model.apply("node", report(interval=10.0), 1000.0)
        self.assertEqual(self.model.attention(1029.0), [])
        items = self.model.attention(1031.0)
        self.assertEqual(len(items), 1)
        self.assertIn("metrics stopped", items[0]["title"])
        self.assertEqual(self.model.snapshot(1031.0)["adapters"][0]["status"], SILENT)

    def test_the_silent_factor_can_be_changed(self):
        model = MetricsModel(silent_factor=1.5)
        model.apply("node", report(interval=10.0), 1000.0)
        self.assertEqual(len(model.attention(1016.0)), 1)

    def test_each_adapter_is_judged_on_its_own(self):
        self.model.apply("a", report(fleet="amr_fleet"), 1000.0)
        self.model.apply("a", report(fleet="amr_fleet", dropped=1), 1060.0)
        self.model.apply("b", report(fleet="tb3_fleet"), 1000.0)
        self.model.apply("b", report(fleet="tb3_fleet"), 1060.0)
        items = self.model.attention(1060.0)
        self.assertEqual(len(items), 1)
        self.assertIn("amr_fleet", items[0]["title"])


class Presence(unittest.TestCase):
    def setUp(self):
        self.model = MetricsModel(started=1000.0, grace_s=10.0)
        self.model.expect("node", "tb3_fleet")

    def status(self, now):
        return self.model.snapshot(now)["adapters"][0]["status"]

    def test_an_adapter_whose_topic_has_a_publisher_is_found_before_its_first_report(self):
        self.model.set_present("node", True)
        self.assertEqual(self.status(1002.0), FOUND)
        self.assertEqual(self.model.attention(1002.0), [])

    def test_nothing_is_missing_while_ros_is_still_discovering(self):
        self.model.set_present("node", False)
        self.assertEqual(self.status(1005.0), WAITING)
        self.assertEqual(self.model.attention(1005.0), [])

    def test_an_adapter_nobody_publishes_for_is_missing_after_the_grace_period(self):
        self.model.set_present("node", False)
        self.assertEqual(self.status(1011.0), ABSENT)
        item = self.model.attention(1011.0)[0]
        self.assertEqual(item["severity"], "warning")
        self.assertIn("tb3_fleet adapter not found", item["title"])
        self.assertIn("/node/metrics", item["detail"])

    def test_an_adapter_that_reported_and_then_lost_its_publisher_is_missing_at_once(self):
        self.model.apply("node", report(), 1020.0)
        self.model.set_present("node", True)
        self.assertEqual(self.status(1021.0), OK)
        self.model.set_present("node", False)
        self.assertEqual(self.status(1022.0), ABSENT)
        self.assertIn("stopped", self.model.attention(1022.0)[0]["detail"])

    def test_the_summary_counts_the_adapters_found(self):
        self.model.expect("other", "amr_fleet")
        self.assertEqual(self.model.summary(1001.0), {"total": 2, "found": 0, "level": "wait"})
        self.model.set_present("node", True)
        self.model.set_present("other", True)
        self.assertEqual(self.model.summary(1002.0), {"total": 2, "found": 2, "level": "ok"})
        self.model.set_present("other", False)
        self.assertEqual(self.model.summary(1002.0)["level"], "wait")     # still within the grace period
        self.assertEqual(self.model.summary(1020.0), {"total": 2, "found": 1, "level": "warn"})
        self.model.set_present("node", False)
        self.assertEqual(self.model.summary(1020.0), {"total": 2, "found": 0, "level": "err"})


if __name__ == "__main__":
    unittest.main()
