"""Operator features: robot controls, robot registration, lane closures, nav graph editing, adapter metrics.

Every action becomes a gateway command (eiu_rmf_gateway/docs/contract.md). The fleet adapters own the rules:
the backend forwards requests, waits for the answer and shows it.
"""

import datetime
import hashlib
import json
import logging
import math
import re
import time
import uuid
import zoneinfo

import yaml
from sqlalchemy import select

from . import views
from .db import Database, Delivery, DeliveryEvent, User
from .rmf.shared import metrics_model
from .service import ApiFail, Service
from .settings import Settings
from .site import Site

log = logging.getLogger("eiu_web.operations")

ROBOT_ACTIONS = ("pause", "resume", "speed_limit", "init_position")
TASK_GROUPS = ("active", "scheduled", "finished", "all")
METRIC_HEALTH = {"ok": "ok", "warning": "warning", "critical": "critical", "silent": "critical", "absent": "critical",
                 "waiting": "unknown", "found": "unknown"}
NUMBERED = re.compile(r"^(.*?)(\d+)$")


def _command_id() -> str:
    return f"eiu-web-op-{uuid.uuid4().hex[:12]}"


def _finite(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def robot_status(robot: dict, task: dict | None) -> str:
    """IDLE, NAVIGATING, EXECUTING, CHARGING, PAUSED, MAINTENANCE, OFFLINE or ERROR."""
    mode = robot["mode"]
    if robot["connection"] == "offline":
        return "OFFLINE"
    if mode in ("emergency", "adapter_error"):
        return "ERROR"
    if robot["maintenance"]:
        return "MAINTENANCE"
    if robot["paused"] or mode == "paused" or (task and task["state"] == "PAUSED"):
        return "PAUSED"
    if mode == "charging":
        return "CHARGING"
    if task and task["state"] == "EXECUTING":
        if task["kind"] != "delivery" or robot["activity"] != "moving":
            return "EXECUTING"
        return "NAVIGATING"
    if mode == "cleaning":
        return "EXECUTING"
    if robot["activity"] == "moving":
        return "NAVIGATING"
    return "IDLE"


def robot_health(robot: dict, alert_level: str | None) -> str:
    """critical (offline, error or a critical alert), warning (a warning alert or maintenance) or healthy."""
    if robot["status"] in ("OFFLINE", "ERROR") or alert_level == "critical":
        return "critical"
    if alert_level == "warning" or robot["status"] == "MAINTENANCE":
        return "warning"
    return "healthy"


def suggest(robot: dict, registries: dict[str, dict]) -> dict:
    """Prefill for registering a discovered robot: its old place if it was removed, else a fleet of its type,
    the next name in that fleet's numbering and a free charger."""
    removed = robot.get("removed_as") or {}
    if removed.get("fleet") in registries:
        return {"fleet": removed["fleet"], "name": removed.get("name", ""), "charger": removed.get("charger", "")}
    fleets = list(registries.values())
    same_type = [r for r in fleets if robot.get("series") and r.get("series") == robot.get("series")]
    registry = (same_type or fleets or [{}])[0]
    names = [r.get("name", "") for r in registry.get("robots", [])]
    numbered = [NUMBERED.match(n) for n in names]
    numbered = [m for m in numbered if m]
    if numbered:
        prefix = max(numbered, key=lambda m: int(m[2]))[1]
        taken = {int(m[2]) for m in numbered if m[1] == prefix}
        name = f"{prefix}{max(taken) + 1}"
    else:
        name = f"{registry.get('fleet', 'robot')}_{len(names) + 1}"
    charger = next((c.get("name", "") for c in registry.get("chargers", []) if not c.get("used_by")), "")
    return {"fleet": registry.get("fleet", ""), "name": name, "charger": charger}


def merge_discovery(snapshots: dict[str, dict]) -> list[dict]:
    """Robots waiting on the broker, merged across the adapters that reported them."""
    seen: dict[tuple, dict] = {}
    for entry in snapshots.values():
        snapshot = entry.get("snapshot") or {}
        for robot in snapshot.get("robots", []):
            key = (robot.get("manufacturer", ""), str(robot.get("serial", "")))
            kept = seen.get(key)
            if kept is None:
                seen[key] = dict(robot, reporters=[snapshot.get("reporter", "")])
                continue
            kept["reporters"].append(snapshot.get("reporter", ""))
            if robot.get("removed_as") and not kept.get("removed_as"):
                kept["removed_as"] = robot["removed_as"]
    return [seen[k] for k in sorted(seen)]


def graph_level(doc: dict, level_id: str) -> dict:
    """Vertices and lanes of one level of a nav graph document, with their extra attributes."""
    level = (doc.get("levels") or {}).get(level_id) or {}
    vertices = []
    for x, y, *rest in level.get("vertices", []):
        attrs = dict((rest[0] if rest else None) or {})
        vertices.append({"name": attrs.pop("name", "") or "", "x": float(x), "y": float(y),
                         "charger": bool(attrs.pop("is_charger", False)), "attrs": attrs})
    lanes = [{"from": int(a), "to": int(b), "attrs": dict((rest[0] if rest else None) or {})}
             for a, b, *rest in level.get("lanes", [])]
    return {"vertices": vertices, "lanes": lanes}


def check_graph(vertices: list, lanes: list) -> tuple[str, str]:
    """('', '') when the edited level is consistent, else (error code, detail)."""
    names = [v.get("name", "") for v in vertices]
    named = [n for n in names if n]
    if len(named) != len(set(named)):
        dup = sorted({n for n in named if named.count(n) > 1})
        return "nav_graph.duplicate_name", ", ".join(dup)
    if not all(_finite(v.get("x")) and _finite(v.get("y")) for v in vertices):
        return "nav_graph.bad_position", ""
    seen = set()
    for lane in lanes:
        a, b = lane.get("from"), lane.get("to")
        if not (isinstance(a, int) and isinstance(b, int) and 0 <= a < len(vertices) and 0 <= b < len(vertices)) or a == b:
            return "nav_graph.bad_lane", f"{a} -> {b}"
        if (a, b) in seen:
            return "nav_graph.duplicate_lane", f"{a} -> {b}"
        seen.add((a, b))
    return "", ""


class Operations:
    def __init__(self, settings: Settings, site: Site, db: Database, rmf, service: Service, alerts=None,
                 maintenance=None):
        self.alerts = alerts
        self.maintenance = maintenance
        # When each robot came online, for its runtime.
        self._online_since: dict[str, int] = {}
        self.settings = settings
        self.site = site
        self.db = db
        self.rmf = rmf
        self.service = service
        self.metrics = metrics_model.MetricsModel(
            capacity=settings.get("metrics", "history_samples"), silent_factor=settings.get("metrics", "silent_factor"),
            started=time.time(), grace_s=settings.get("metrics", "grace_s"))
        self._metrics_seen: dict[str, int] = {}
        self._graph_sha = ""
        self._signature = ""

    @property
    def ops(self) -> dict:
        return getattr(self.rmf, "ops", None) or {}

    # Tick, after the tracker

    def tick(self) -> bool:
        """Fold the gateway's operations state in; True when what operators see changed."""
        now_ms = int(time.time() * 1000)
        for name, robot in self.service._robots.items():
            if robot["stale"]:
                self._online_since.pop(name, None)
            else:
                self._online_since.setdefault(name, now_ms)
        for name in set(self._online_since) - set(self.service._robots):
            del self._online_since[name]
        ops = self.ops
        for node, adapter in (ops.get("adapters") or {}).items():
            self.metrics.expect(node)
            self.metrics.set_present(node, bool(adapter.get("metrics_topic")))
        for node, entry in (ops.get("metrics") or {}).items():
            received = int(entry.get("received_ms", 0))
            if received and self._metrics_seen.get(node) != received:
                self._metrics_seen[node] = received
                self.metrics.apply(node, entry.get("report"), received / 1000.0)
        nav = ops.get("nav_graph")
        if nav and nav.get("sha256") != self._graph_sha:
            self._graph_sha = nav.get("sha256", "")
            self._apply_graph(nav.get("yaml", ""))
        parts = {k: ops.get(k) for k in ("adapters", "controls", "lanes", "registry", "discovery")}
        parts["metrics"] = {n: e.get("received_ms") for n, e in (ops.get("metrics") or {}).items()}
        parts["nav_graph"] = self._graph_sha
        signature = hashlib.sha1(json.dumps(parts, sort_keys=True, default=str).encode()).hexdigest()
        changed = signature != self._signature
        self._signature = signature
        return changed

    def _apply_graph(self, text: str) -> None:
        try:
            doc = yaml.safe_load(text) or {}
        except yaml.YAMLError as e:
            log.warning("nav graph from the gateway is not YAML: %s", e)
            return
        levels = doc.get("levels") or {}
        self.site.set_lane_layout({str(k): {"lanes": (v or {}).get("lanes") or []} for k, v in levels.items()})
        for level_id in self.site.levels:
            if level_id in levels:
                level = graph_level(doc, level_id)
                self.site.update_graph(level_id, [{"name": v["name"], "x": v["x"], "y": v["y"], "charger": v["charger"]}
                                                  for v in level["vertices"]],
                                       [[l["from"], l["to"]] for l in level["lanes"]])

    # Commands

    def _send(self, cmd_type: str, body: dict, *, final_only: bool = False, timeout_s: float | None = None) -> dict:
        if self.rmf is None or not getattr(self.rmf, "available", False):
            raise ApiFail(503, "rmf.unavailable")
        cmd_id = _command_id()
        self.service.waiter.expect(cmd_id, final_only=final_only)
        if not self.rmf.command(cmd_type, cmd_id, body):
            self.service.waiter.wait(cmd_id, 0)
            raise ApiFail(503, "rmf.unavailable")
        result = self.service.waiter.wait(cmd_id, timeout_s or self.settings.get("operations", "command_timeout_s"))
        if result is None:
            raise ApiFail(504, "command.no_answer")
        if not result.get("ok"):
            raise ApiFail(409, f"command.{result.get('error') or 'refused'}", result.get("message", ""))
        return result

    def _audit(self, user, action: str, target: str, detail: str = "") -> None:
        with self.db.session() as s:
            self.db.audit(s, user.id, action, target, detail)
            s.commit()

    # Robots

    def silent_robots(self) -> set[str]:
        """Robots RMF still reports but whose adapter has not heard them within its state timeout."""
        out = set()
        for adapter in self.metrics.snapshot(time.time())["adapters"]:
            r = adapter["robots"]
            if (adapter["status"] in ("ok", "warning", "critical") and r["online"] < r["registered"]
                    and r["oldest_state_robot"] and r["state_age_max_s"] > (r.get("state_timeout_s") or 0)):
                out.add(r["oldest_state_robot"])
        return out

    def robots(self, user=None) -> list[dict]:
        """Robots with their status, service type, current task and health; only those the user may see."""
        controls = self.ops.get("controls") or {}
        silent = self.silent_robots()
        registry, access = self.service.registry, self.service.access
        received = getattr(self.rmf, "fleets_received", {}) if self.rmf else {}
        in_maintenance = self.maintenance.active if self.maintenance else set()
        levels = self.alerts.robot_levels if self.alerts else {}
        out = []
        for robot in self.service._robots.values():
            if user is not None and access is not None and not access.fleet_visible(user, robot["fleet"]):
                continue
            control = controls.get(robot["name"]) or {}
            if robot["name"] in silent:
                robot = dict(robot, stale=True)
            delivery_id = self.service._task_to_delivery.get(robot["task_id"])
            task = self.service.live_tasks.get(delivery_id) if delivery_id else None
            level_id = self.site.level_of_rmf(robot["level"])
            near = self.site.nearest_location(level_id, robot["x"], robot["y"])
            item = {
                "name": robot["name"], "fleet": robot["fleet"], "levelId": level_id,
                "x": round(robot["x"], 3), "y": round(robot["y"], 3), "yaw": round(robot["yaw"], 3),
                "battery": round(robot["battery"], 1), "activity": "offline" if robot["stale"] else robot["activity"],
                "mode": "offline" if robot["stale"] else robot.get("mode", robot["activity"]),
                "taskId": robot["task_id"] or None, "deliveryId": delivery_id,
                "adapter": control.get("node") or None,
                "controls": bool(control.get("available")),
                "speedLimit": control.get("speed_limit"),
                "paused": control.get("paused"),
                "serviceType": registry.fleet_service(robot["fleet"]) if registry else None,
                "services": registry.fleet_services(robot["fleet"]) if registry else [],
                "capabilities": registry.fleet_capabilities(robot["fleet"]) if registry else [],
                "connection": "offline" if robot["stale"] else "online",
                "onlineSince": self._online_since.get(robot["name"]),
                "lastUpdateAt": received.get(robot["fleet"]) or None,
                "locationId": near["id"] if near else None,
                "task": task,
                "maintenance": robot["name"] in in_maintenance,
                "telemetry": dict(robot.get("telemetry") or {}),
            }
            item["status"] = robot_status(item, task)
            item["health"] = robot_health(item, levels.get(robot["name"]))
            out.append(item)
        return sorted(out, key=lambda r: (r["fleet"], r["name"]))

    def control(self, user, robot: str, action: str, body: dict) -> dict:
        if robot not in self.service._robots:
            raise ApiFail(404, "robot.not_found")
        if action == "pause":
            cmd = ("robot_pause", {"robot": robot})
        elif action == "resume":
            cmd = ("robot_resume", {"robot": robot})
        elif action == "speed_limit":
            mps = body.get("mps")
            if not _finite(mps) or mps < 0:
                raise ApiFail(422, "robot.invalid_speed")
            cmd = ("robot_speed_limit", {"robot": robot, "mps": float(mps)})
        elif action == "init_position":
            pose = dict(body)
            if pose.get("waypoint"):
                level = self.site.levels.get(self.site.level_of_rmf(self.service._robots[robot]["level"]))
                vertex = next((v for v in (level.vertices if level else []) if v["name"] == pose["waypoint"]), None)
                if vertex is None:
                    raise ApiFail(422, "robot.unknown_waypoint")
                pose.update(x=vertex["x"], y=vertex["y"])
            if not all(_finite(pose.get(k)) for k in ("x", "y", "yaw")):
                raise ApiFail(422, "robot.invalid_pose")
            cmd = ("robot_init_position", {"robot": robot, "x": float(pose["x"]), "y": float(pose["y"]),
                                           "yaw": float(pose["yaw"])})
        else:
            raise ApiFail(404, "robot.unknown_action")
        previous = ((self.ops.get("controls") or {}).get(robot) or {}).get("speed_limit")
        result = self._send(*cmd)
        detail = dict(cmd[1], previous_mps=previous) if action == "speed_limit" else cmd[1]
        self._audit(user, f"robot.{action}", robot, json.dumps(detail))
        return {"ok": True, "message": result.get("message", "")}

    # Overview and tasks

    def overview(self) -> dict:
        robots = self.robots()
        online = [r for r in robots if r["activity"] != "offline"]
        robot_counts = {"total": len(robots), "online": len(online), "offline": len(robots) - len(online),
                        "moving": sum(1 for r in online if r["activity"] == "moving"),
                        "idle": sum(1 for r in online if r["activity"] == "idle" and r["mode"] != "charging"),
                        "charging": sum(1 for r in online if r["mode"] == "charging"),
                        "waiting": sum(1 for r in online if r["activity"] == "waiting"),
                        "paused": sum(1 for r in online if r["paused"])}
        day_start = self._day_start()
        with self.db.session() as s:
            rows = s.scalars(select(Delivery).where((Delivery.status.not_in(views.FINAL))
                                                    | (Delivery.finished_at >= day_start))).all()
        active = [r for r in rows if r.status in views.ACTIVE]
        task_counts = {"active": len(active), "queued": sum(1 for r in rows if r.status == "queued"),
                       "scheduled": sum(1 for r in rows if r.status == "scheduled"),
                       "completedToday": sum(1 for r in rows if r.status == "completed"),
                       "failedToday": sum(1 for r in rows if r.status == "failed"),
                       "cancelledToday": sum(1 for r in rows if r.status == "cancelled"),
                       "byKind": {k: sum(1 for r in active if r.kind == k) for k in sorted({r.kind for r in active})}}
        alerts = self.alerts.list("open")["counts"] if self.alerts else {"open": 0, "unacked": 0, "critical": 0}
        return {"robots": robot_counts, "tasks": task_counts, "alerts": alerts, "health": self.health()}

    def _day_start(self) -> int:
        tz = zoneinfo.ZoneInfo(self.settings.get("site", "time_zone"))
        start = datetime.datetime.now(tz).replace(hour=0, minute=0, second=0, microsecond=0)
        return int(start.timestamp() * 1000)

    def health(self) -> list[dict]:
        """Status of each part of the chain from the backend to the robots (ok, warning, critical or unknown),
        with the figures behind it in `details` (keys ending in _s are durations in seconds)."""
        status = self.service.rmf_status()
        gateway = getattr(self.rmf, "gateway", {}) if self.rmf else {}
        received = getattr(self.rmf, "fleets_received", {}) if self.rmf else {}
        now_s = time.time()
        robots = self.service._robots
        registry = self.ops.get("registry") or {}

        def age(ms) -> float | None:
            return round(now_s - ms / 1000, 1) if ms else None

        items = [{"key": "database", "status": "ok", "params": {}, "details": {"engine": "SQLite"}},
                 {"key": "gateway", "status": {"disabled": "unknown", "unavailable": "critical"}.get(status, "ok"),
                  "params": {"domain": gateway.get("ros_domain_id", "")},
                  "details": {"heartbeat_age_s": age(gateway.get("at_ms")), "uptime_s": age(gateway.get("started_ms")),
                              "ros_domain": gateway.get("ros_domain_id"), "nav_graph_path": gateway.get("nav_graph_path") or None}},
                 {"key": "rmf", "status": {"online": "ok", "offline": "warning", "unavailable": "critical"}
                  .get(status, "unknown"), "params": {},
                  "details": {"fleets_reporting": sum(1 for f in received if age(received[f]) is not None
                                                      and age(received[f]) <= self.settings.get("rmf", "fleet_offline_s")),
                              "robots": len(robots)}}]
        if gateway:
            items.append({"key": "task_events", "status": "ok" if gateway.get("task_events") else "unknown", "params": {},
                          "details": {"enabled": bool(gateway.get("task_events"))}})
        fleets: dict[str, list] = {}
        for robot in robots.values():
            fleets.setdefault(robot["fleet"], []).append(robot)
        for fleet in sorted(set(fleets) | set(registry)):
            members = fleets.get(fleet, [])
            fresh = [r for r in members if not r["stale"]]
            reg = (registry.get(fleet) or {}).get("registry") or {}
            items.append({"key": "fleet", "status": "ok" if fresh and len(fresh) == len(members)
                          else "warning" if fresh else "critical",
                          "params": {"name": fleet, "online": len(fresh), "total": len(members)},
                          "details": {"state_age_s": age(received.get(fleet)), "online": len(fresh), "total": len(members),
                                      "adapter": reg.get("adapter_node"), "interface": reg.get("interface")}})
        for adapter in self.metrics.snapshot(now_s)["adapters"]:
            r, totals, mqtt = adapter["robots"], adapter["totals"], adapter["mqtt"]
            reported = adapter["reported_ago_s"] is not None
            items.append({"key": "adapter", "status": METRIC_HEALTH.get(adapter["status"], "unknown"),
                          "params": {"name": adapter["fleet"], "node": adapter["node"], "state": adapter["status"]},
                          "details": {"node": adapter["node"], "reported_ago_s": adapter["reported_ago_s"],
                                      "interval_s": adapter["interval_s"] if reported else None,
                                      "uptime_s": adapter["uptime_s"] if reported else None,
                                      "robots_online": f"{r['online']}/{r['registered']}" if reported else None,
                                      "messages_rx": totals["rx"] if reported else None,
                                      "messages_dropped": totals["dropped"] if reported else None,
                                      "publish_failed": totals["published_failed"] if reported else None}})
            if reported:
                items.append({"key": "mqtt", "status": "ok" if mqtt["connected"] else "critical",
                              "params": {"name": adapter["fleet"]},
                              "details": {"connected": mqtt["connected"], "connections_lost": mqtt["connections_lost"]}})
        return items

    def tasks(self, user, group: str, kind: str = "", query: str = "") -> dict:
        if group not in TASK_GROUPS:
            raise ApiFail(422, "tasks.invalid_group")
        limit = self.settings.get("operations", "tasks_limit")
        with self.db.session() as s:
            open_rows = s.scalars(select(Delivery).where(Delivery.status.not_in(views.FINAL))).all()
            finished = s.scalars(select(Delivery).where(Delivery.status.in_(views.FINAL))
                                 .order_by(Delivery.created_at.desc()).limit(limit)).all()
            rows = sorted([*open_rows, *finished], key=lambda r: -r.created_at)
            people = {u.id: u for u in s.scalars(select(User)).all()}
            groups = {"active": lambda r: r.status in views.ACTIVE, "scheduled": lambda r: r.status == "scheduled",
                      "finished": lambda r: r.status in views.FINAL, "all": lambda r: True}
            counts = {g: sum(1 for r in rows if groups[g](r)) for g in TASK_GROUPS}
            needle = query.strip().lower()

            def matches(r: Delivery) -> bool:
                if kind and r.kind != kind:
                    return False
                if not needle:
                    return True
                person = people.get(r.requester_id)
                places = [r.pickup_id, r.dropoff_id, *(r.stops or [])]
                names = [n for p in places for n in (self.site.location(p) or {}).get("name", {}).values()]
                hay = [str(r.id), f"#{r.id}", r.robot or "", r.fleet or "", r.rmf_task_id or "",
                       person.full_name if person else "", person.email if person else "", *places, *names]
                return any(needle in h.lower() for h in hay)

            picked = [r for r in rows if groups[group](r) and matches(r)][:limit]
            events: dict[int, list] = {}
            if picked:
                for e in s.scalars(select(DeliveryEvent).where(DeliveryEvent.delivery_id.in_([r.id for r in picked]))):
                    events.setdefault(e.delivery_id, []).append(e)
            items = [views.delivery(self.site, r, events.get(r.id, []), people.get(r.requester_id), user, operator=True)
                     | {"rmfTaskId": r.rmf_task_id} for r in picked]
            summary, unassigned, late = self._task_watch(open_rows, finished)
        if group == "active":
            rank = {"queued": 0}
            items.sort(key=lambda d: (rank.get(d["status"], 1), -d["createdAt"]))
        return {"items": items, "counts": counts, "summary": summary, "unassigned": unassigned, "late": late}

    def _task_watch(self, open_rows: list, finished: list) -> tuple[dict, list, list]:
        """Counts of today, tasks still without a robot, and running tasks past their ETA by `operations.late_after_s`."""
        now = int(time.time() * 1000)
        day_start = self._day_start()
        late_ms = int(self.settings.get("operations", "late_after_s") * 1000)
        today = [r for r in finished if (r.finished_at or 0) >= day_start]
        done = [(r.finished_at - max(r.created_at, r.scheduled_at or 0)) / 60000 for r in today if r.status == "completed"]

        def ref(r) -> dict:
            since = max(r.created_at, r.scheduled_at or 0)
            return {"id": r.id, "kind": r.kind, "service": r.service or r.kind, "pickupId": r.pickup_id, "dropoffId": r.dropoff_id, "stops": r.stops or [],
                    "robot": r.robot, "since": since, "etaAt": r.eta_at}

        unassigned = sorted((ref(r) for r in open_rows if r.status == "queued" and not r.robot), key=lambda t: t["since"])
        late = sorted((ref(r) | {"lateMin": round((now - r.eta_at) / 60000, 1)} for r in open_rows
                       if r.status in views.ACTIVE and r.eta_at and now - r.eta_at > late_ms), key=lambda t: -t["lateMin"])
        summary = {"active": sum(1 for r in open_rows if r.status in views.ACTIVE), "queued": len(unassigned),
                   "scheduled": sum(1 for r in open_rows if r.status == "scheduled"),
                   "completedToday": sum(1 for r in today if r.status == "completed"),
                   "failedToday": sum(1 for r in today if r.status == "failed"),
                   "avgDurationMin": round(sum(done) / len(done), 1) if done else None, "late": len(late)}
        return summary, unassigned, late

    def cancel_task(self, user, task_id: int) -> dict:
        with self.db.session() as s:
            row = s.get(Delivery, task_id)
            if row is None:
                raise ApiFail(404, "delivery.not_found")
            viewer = s.get(User, user.id)
            row = self.service.cancel(s, viewer, row, operator=True)
            events = s.scalars(select(DeliveryEvent).where(DeliveryEvent.delivery_id == row.id)).all()
            return views.delivery(self.site, row, events, s.get(User, row.requester_id), viewer, operator=True)

    # Registration

    def registration(self) -> dict:
        registries = {fleet: entry.get("registry") or {} for fleet, entry in (self.ops.get("registry") or {}).items()}
        pending = merge_discovery(self.ops.get("discovery") or {})
        for robot in pending:
            robot["suggestion"] = suggest(robot, registries)
        return {"fleets": [registries[f] for f in sorted(registries)], "pending": pending}

    def register(self, user, body: dict) -> dict:
        action = body.get("action")
        fleet, name = body.get("fleet"), body.get("name")
        if action not in ("check", "add", "remove") or not isinstance(fleet, str) or not fleet \
                or not isinstance(name, str) or not name:
            raise ApiFail(422, "registration.invalid")
        if action == "remove":
            request = {"action": "remove", "fleet": fleet, "name": name}
        else:
            for key in ("manufacturer", "serial", "charger"):
                if not isinstance(body.get(key), str) or not body.get(key):
                    raise ApiFail(422, "registration.invalid")
            request = {"action": "add", "fleet": fleet, "name": name, "manufacturer": body["manufacturer"],
                       "serial": body["serial"], "charger": body["charger"],
                       "responsive_wait": bool(body.get("responsiveWait", False)),
                       "confirm_unverified": bool(body.get("confirmUnverified", False)),
                       "dry_run": action == "check"}
        result = self._send("registration_request", {"request": request}, final_only=True,
                            timeout_s=self.settings.get("operations", "registration_timeout_s"))
        verdict = result.get("result") or {}
        if action != "check":
            self._audit(user, f"registration.{action}", f"{fleet}/{name}",
                        json.dumps({"ok": verdict.get("ok"), "request": request}))
        return {"ok": bool(verdict.get("ok")), "dryRun": bool(verdict.get("dry_run")),
                "persisted": bool(verdict.get("persisted")), "needsConfirmation": bool(verdict.get("needs_confirmation")),
                "errors": verdict.get("errors") or [], "warnings": verdict.get("warnings") or []}

    # Lanes

    def lanes(self) -> dict:
        return {"fleets": {fleet: entry.get("closed_lanes", []) for fleet, entry in (self.ops.get("lanes") or {}).items()},
                "offsets": dict(self.site.lane_offsets)}

    def set_lanes(self, user, body: dict) -> dict:
        close, open_ = body.get("close") or [], body.get("open") or []
        for i in close + open_:
            if not isinstance(i, int) or isinstance(i, bool) or not 0 <= i < self.site.lane_total:
                raise ApiFail(422, "lanes.invalid")
        if not close and not open_:
            raise ApiFail(422, "lanes.invalid")
        fleets = [body["fleet"]] if isinstance(body.get("fleet"), str) and body["fleet"] else \
            sorted({r["fleet"] for r in self.service._robots.values()} | set((self.ops.get("registry") or {})))
        if not fleets:
            raise ApiFail(409, "lanes.no_fleet")
        for fleet in fleets:
            self._send("lane_request", {"fleet": fleet, "close_lanes": close, "open_lanes": open_})
        self._audit(user, "lanes.request", ",".join(fleets), json.dumps({"close": close, "open": open_}))
        return {"ok": True, "fleets": fleets}

    # Nav graph

    def nav_graph(self, level_id: str | None) -> dict:
        nav = self.ops.get("nav_graph")
        if not nav:
            return {"available": False}
        doc = yaml.safe_load(nav.get("yaml", "")) or {}
        levels = list((doc.get("levels") or {}).keys())
        level_id = level_id if level_id in levels else next((l for l in self.site.levels if l in levels), levels[0] if levels else "")
        return {"available": True, "path": nav.get("path", ""), "sha256": nav.get("sha256", ""), "levels": levels,
                "levelId": level_id, **graph_level(doc, level_id)}

    def save_nav_graph(self, user, body: dict) -> dict:
        nav = self.ops.get("nav_graph")
        if not nav:
            raise ApiFail(409, "nav_graph.unavailable")
        if body.get("baseSha256") != nav.get("sha256"):
            raise ApiFail(409, "nav_graph.stale")
        level_id, vertices, lanes = body.get("levelId"), body.get("vertices"), body.get("lanes")
        doc = yaml.safe_load(nav.get("yaml", "")) or {}
        if level_id not in (doc.get("levels") or {}) or not isinstance(vertices, list) or not isinstance(lanes, list):
            raise ApiFail(422, "nav_graph.invalid")
        error, detail = check_graph(vertices, lanes)
        if error:
            raise ApiFail(422, error, detail)
        names = {v.get("name") for v in vertices}
        missing = [l["waypoint"] for l in self.site.locations if l["levelId"] == level_id and l["waypoint"] not in names]
        if missing:
            raise ApiFail(422, "nav_graph.catalog_waypoint_missing", ", ".join(missing))

        level = doc["levels"][level_id]
        level["vertices"] = []
        for v in vertices:
            attrs = {k: val for k, val in dict(v.get("attrs") or {}).items() if k not in ("name", "is_charger")}
            if v.get("name"):
                attrs["name"] = v["name"]
            if v.get("charger"):
                attrs["is_charger"] = True
            level["vertices"].append([round(float(v["x"]), 4), round(float(v["y"]), 4), attrs])
        level["lanes"] = [[int(l["from"]), int(l["to"]), dict(l.get("attrs") or {})] for l in lanes]
        text = yaml.safe_dump(doc, sort_keys=False, default_flow_style=None, allow_unicode=True)
        result = self._send("nav_graph_save", {"yaml": text, "base_sha256": nav["sha256"]})
        self._audit(user, "nav_graph.save", nav.get("path", ""),
                    json.dumps({"level": level_id, "vertices": len(vertices), "lanes": len(lanes)}))
        return {"ok": True, "sha256": result.get("message", "")}

    # System

    def system(self) -> dict:
        now = time.time()
        return {"rmf": self.service.rmf_status(), "gateway": getattr(self.rmf, "gateway", {}) if self.rmf else {},
                "summary": self.metrics.summary(now), "attention": self.metrics.attention(now),
                **self.metrics.snapshot(now)}
