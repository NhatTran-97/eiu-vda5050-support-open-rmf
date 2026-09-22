"""Turn the fleet adapters' metrics reports into chart series and attention items.

Reports are JSON on '/<adapter node>/metrics' (see the adapter's docs/architecture.md). Counts grow from the adapter's
start and latencies cover one reporting interval, so per-interval rates are derived from the counts. No Qt or ROS;
the limits compared against come from the report itself.
"""

from collections import deque

# Samples kept per adapter for the charts.
DEFAULT_CAPACITY = 120
# An adapter is silent after this many reporting intervals without a report.
DEFAULT_SILENT_FACTOR = 3.0
# Start-up grace (s) before an adapter with no publisher counts as missing; ROS discovery takes a moment.
DEFAULT_GRACE_S = 10.0

_RX_KINDS = ("state", "visualization", "connection", "factsheet")
_DROP_KINDS = ("bad_payload", "bad_topic", "invalid_state", "stale_state", "oversize")

OK, WARNING, CRITICAL, SILENT, WAITING, FOUND, ABSENT = "ok", "warning", "critical", "silent", "waiting", "found", "absent"


def _number(source, *path, default=0.0):
    """The number at `path` inside nested dicts, or `default` when anything on the way is missing or not a number."""
    value = source
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return default
        value = value[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return float(value)


def _text(source, *path, default=""):
    value = source
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return default
        value = value[key]
    return value if isinstance(value, str) else default


def _flag(source, *path, default=False):
    value = source
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return default
        value = value[key]
    return value if isinstance(value, bool) else default


def _count_sum(report, group, kinds):
    return sum(_number(report, group, kind) for kind in kinds)


def _grew(current, previous):
    """How much a count that only grows went up; a count that went down means the adapter restarted."""
    return current - previous if current >= previous else current


class AdapterHistory:
    """What one adapter reported: its latest report, what changed since the report before, and a series for charts."""

    def __init__(self, node, fleet="", capacity=DEFAULT_CAPACITY):
        self.node = node
        self.fleet = fleet
        # Whether something publishes the node's metrics topic right now; None until known.
        self.present = None
        self.latest = None
        self.received = None
        self.samples = deque(maxlen=max(2, int(capacity)))
        # Growth since the previous report, by name.
        self.delta = {}

    def add(self, report, received):
        interval = _number(report, "interval_s")
        received_gap = received - self.received if self.received is not None else 0.0
        seconds = interval if interval > 0 else received_gap
        previous = self.latest or {}

        rx = _count_sum(report, "rx", _RX_KINDS)
        rx_before = _count_sum(previous, "rx", _RX_KINDS)
        dropped = {kind: _grew(_number(report, "dropped", kind), _number(previous, "dropped", kind)) for kind in _DROP_KINDS}
        self.delta = {
            "rx": _grew(rx, rx_before) if previous else 0.0,
            "dropped": dropped if previous else {kind: 0.0 for kind in _DROP_KINDS},
            "publish_failed": _grew(_number(report, "published", "failed"), _number(previous, "published", "failed")) if previous else 0.0,
            "overruns": _grew(_number(report, "update_loop", "overruns"), _number(previous, "update_loop", "overruns")) if previous else 0.0,
            "connections_lost": _grew(_number(report, "mqtt", "connections_lost"), _number(previous, "mqtt", "connections_lost")) if previous else 0.0,
        }
        if not self.fleet:
            self.fleet = _text(report, "fleet")

        rate = self.delta["rx"] / seconds if previous and seconds > 0 else None
        self.samples.append({
            "t": received,
            "msg_per_s": rate,
            "handle_p99_us": _number(report, "latency_us", "handle_state", "p99") if _number(report, "latency_us", "handle_state", "count") else None,
            "state_age_max_s": _number(report, "robots", "state_age_max_s"),
            "loop_p99_ms": _number(report, "update_loop", "pass_us", "p99") / 1000.0 if _number(report, "update_loop", "pass_us", "count") else None,
        })
        self.latest = report
        self.received = received

    def interval(self):
        return _number(self.latest, "interval_s") if self.latest else 0.0

    def silent(self, now, factor):
        if self.received is None:
            return False
        interval = self.interval()
        return interval > 0 and now - self.received > factor * interval


class MetricsModel:
    """The adapters the dashboard follows, with their histories, ready for QML."""

    def __init__(self, capacity=DEFAULT_CAPACITY, silent_factor=DEFAULT_SILENT_FACTOR, started=0.0, grace_s=DEFAULT_GRACE_S):
        self._capacity = capacity
        self._silent_factor = silent_factor
        self._started = started
        self._grace_s = grace_s
        self._adapters = {}

    def expect(self, node, fleet=""):
        """Follow an adapter node even before it reports."""
        if node and node not in self._adapters:
            self._adapters[node] = AdapterHistory(node, fleet, self._capacity)

    def set_present(self, node, present):
        """Record whether something publishes the node's metrics topic."""
        self.expect(node)
        self._adapters[node].present = bool(present)

    def apply(self, node, report, now):
        """Record a report received at time `now` (seconds); anything that is not a JSON object is ignored."""
        if not isinstance(report, dict):
            return
        self.expect(node)
        self._adapters[node].add(report, now)

    def nodes(self):
        return list(self._adapters)

    def status_of(self, history, now):
        if history.present is False and now - self._started >= self._grace_s:
            return ABSENT
        if history.latest is None:
            return FOUND if history.present else WAITING
        if history.silent(now, self._silent_factor):
            return SILENT
        if not _flag(history.latest, "mqtt", "connected", default=True):
            return CRITICAL
        d = history.delta
        if sum(d["dropped"].values()) > 0 or d["overruns"] > 0 or d["publish_failed"] > 0:
            return WARNING
        return OK

    def attention(self, now):
        """Items for the Needs Attention list, in the dashboard's own format."""
        items = []
        for history in self._adapters.values():
            status = self.status_of(history, now)
            name = history.fleet or history.node
            if status == ABSENT:
                since = "Its metrics stopped" if history.latest is not None else "Nothing"
                items.append({"severity": "warning", "robot": "", "title": f"{name} adapter not found",
                              "detail": f"{since} publishes /{history.node}/metrics; is the fleet adapter running on this ROS domain?"})
                continue
            if status == SILENT:
                items.append({"severity": "warning", "robot": "", "title": f"{name} adapter metrics stopped",
                              "detail": f"No report for {now - history.received:.0f} s, it reports every {history.interval():.0f} s"})
                continue
            if history.latest is None:
                continue
            if status == CRITICAL:
                items.append({"severity": "critical", "robot": "", "title": f"{name} adapter lost the MQTT broker",
                              "detail": "The adapter cannot reach its robots; it reconnects on its own"})
            d = history.delta
            dropped = {kind: n for kind, n in d["dropped"].items() if n > 0}
            if dropped:
                kinds = ", ".join(f"{kind.replace('_', ' ')} {int(n)}" for kind, n in dropped.items())
                items.append({"severity": "warning", "robot": "", "title": f"{name} adapter dropped {int(sum(dropped.values()))} message(s)",
                              "detail": f"In the last report: {kinds}"})
            if d["overruns"] > 0:
                period = _number(history.latest, "update_loop", "period_ms")
                slow = _number(history.latest, "update_loop", "pass_us", "max") / 1000.0
                items.append({"severity": "warning", "robot": "", "title": f"{name} update loop fell behind",
                              "detail": f"{int(d['overruns'])} pass(es) over the {period:.0f} ms period, slowest {slow:.1f} ms"})
            if d["publish_failed"] > 0:
                items.append({"severity": "warning", "robot": "", "title": f"{name} adapter could not send {int(d['publish_failed'])} message(s)",
                              "detail": "The MQTT connection refused them"})
        return items

    def snapshot(self, now):
        """Everything the System view shows, as plain data."""
        adapters = []
        for history in self._adapters.values():
            report = history.latest or {}
            samples = list(history.samples)
            adapters.append({
                "node": history.node,
                "fleet": history.fleet or history.node,
                "status": self.status_of(history, now),
                "present": history.present,
                "reported_ago_s": None if history.received is None else round(now - history.received, 1),
                "interval_s": history.interval(),
                "uptime_s": _number(report, "uptime_s"),
                "robots": {
                    "registered": int(_number(report, "robots", "registered")),
                    "online": int(_number(report, "robots", "online")),
                    "state_age_max_s": _number(report, "robots", "state_age_max_s"),
                    "oldest_state_robot": _text(report, "robots", "oldest_state_robot"),
                    "state_timeout_s": _number(report, "robots", "state_timeout_s"),
                },
                "mqtt": {"connected": _flag(report, "mqtt", "connected"),
                         "connections_lost": int(_number(report, "mqtt", "connections_lost"))},
                "totals": {"rx": int(_count_sum(report, "rx", _RX_KINDS)), "unregistered": int(_number(report, "rx", "unregistered")),
                           "dropped": int(_count_sum(report, "dropped", _DROP_KINDS)),
                           "published_failed": int(_number(report, "published", "failed"))},
                "delta": {"dropped": int(sum(history.delta.get("dropped", {}).values())) if history.delta else 0,
                          "overruns": int(history.delta.get("overruns", 0)) if history.delta else 0},
                "period_ms": _number(report, "update_loop", "period_ms"),
                "latency_us": {"handle_state": _distribution(report, "latency_us", "handle_state"),
                               "mutex_wait": _distribution(report, "latency_us", "mutex_wait"),
                               "loop_pass": _distribution(report, "update_loop", "pass_us")},
                "series": {
                    "age_s": [round(now - s["t"], 1) for s in samples],
                    "msg_per_s": [s["msg_per_s"] for s in samples],
                    "handle_p99_us": [s["handle_p99_us"] for s in samples],
                    "state_age_max_s": [s["state_age_max_s"] for s in samples],
                    "loop_p99_ms": [s["loop_p99_ms"] for s in samples],
                },
            })
        return {"adapters": adapters}

    def summary(self, now):
        """How many of the followed adapters are found, and the level for a status chip: wait, ok, warn or err."""
        histories = list(self._adapters.values())
        found = sum(1 for h in histories if h.present)
        missing = sum(1 for h in histories if self.status_of(h, now) == ABSENT)
        if not histories or all(h.present is None for h in histories):
            level = "wait"
        elif missing == 0 and found == len(histories):
            level = "ok"
        elif found == 0:
            level = "err"
        else:
            level = "warn" if missing else "wait"
        return {"total": len(histories), "found": found, "level": level}


def _distribution(report, *path):
    return {key: _number(report, *path, key) for key in ("count", "mean", "p50", "p99", "max")}
