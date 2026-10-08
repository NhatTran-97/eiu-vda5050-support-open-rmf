"""What each viewer sees across services: the operations overview, tasks, schedule, task activity, the service and
place catalog, and (admins) the integrations. Every list is filtered by the viewer's access (access.py)."""

import datetime
import time
import zoneinfo
from urllib.parse import urlparse

from sqlalchemy import select

from . import views
from .access import Access
from .db import AuditLog, Database, Delivery, DeliveryEvent, User
from .errors import ApiFail
from .settings import Settings
from .site import Site

TASK_STATES = ("SCHEDULED", "QUEUED", "ASSIGNED", "EXECUTING", "PAUSED", "COMPLETED", "CANCELLED", "FAILED")
OPEN_STATES = frozenset({"QUEUED", "ASSIGNED", "EXECUTING", "PAUSED"})
TASK_GROUPS = ("active", "scheduled", "finished", "all")
SCHEDULE_STATUS = {"COMPLETED": "completed", "EXECUTING": "in_progress", "PAUSED": "in_progress",
                   "SCHEDULED": "scheduled", "QUEUED": "pending", "ASSIGNED": "pending", "FAILED": "failed",
                   "CANCELLED": "cancelled"}
ROBOT_GROUPS = {"NAVIGATING": "active", "EXECUTING": "active", "IDLE": "idle", "CHARGING": "charging",
                "PAUSED": "paused", "MAINTENANCE": "maintenance", "OFFLINE": "offline", "ERROR": "error"}
ACTIVITY_RANGES = {"today": ("hour", 24), "week": ("day", 7), "month": ("day", 30)}
ATTENTION_ROWS = 8
INTEGRATION_LOGS = 12
LOG_PREFIXES = {"open_rmf": ("rmf.", "fleet.", "task.failed"), "ros2_gateway": ("rmf.unavailable", "nav_graph.", "lanes."),
                "vda5050": ("adapter.", "robot.", "registration."), "mqtt": ("adapter.attention",)}


class Dashboard:
    def __init__(self, settings: Settings, site: Site, db: Database, service, operations, alerts, maintenance,
                 history, access: Access):
        self.settings = settings
        self.site = site
        self.db = db
        self.service = service
        self.operations = operations
        self.alerts = alerts
        self.maintenance = maintenance
        self.history = history
        self.access = access

    # Time

    def _tz(self):
        return zoneinfo.ZoneInfo(self.settings.get("site", "time_zone"))

    def day_start(self, now_ms: int | None = None) -> int:
        now = datetime.datetime.fromtimestamp((now_ms or time.time() * 1000) / 1000, self._tz())
        return int(now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)

    # Rows

    def _visible_rows(self, s, user: User, since: int | None = None) -> list[Delivery]:
        """Open tasks and tasks finished after `since` (or the newest `operations.tasks_limit`), that the user sees."""
        open_rows = s.scalars(select(Delivery).where(Delivery.status.not_in(views.FINAL))).all()
        stmt = select(Delivery).where(Delivery.status.in_(views.FINAL))
        if since is not None:
            stmt = stmt.where((Delivery.finished_at >= since) | (Delivery.created_at >= since))
        else:
            stmt = stmt.order_by(Delivery.created_at.desc()).limit(self.settings.get("operations", "tasks_limit"))
        rows = [*open_rows, *s.scalars(stmt).all()]
        return [r for r in rows if self.access.task_visible(user, r)]

    def visible_alerts(self, user: User, state: str = "open") -> dict:
        listing = self.alerts.list(state)
        fleets = {r["name"]: r["fleet"] for r in self.service._robots.values()}
        ids = [a["deliveryId"] for a in listing["items"] if a["deliveryId"] is not None]
        with self.db.session() as s:
            services = {r.id: r.service or r.kind for r in s.scalars(select(Delivery).where(Delivery.id.in_(ids)))} if ids else {}
        items = [a for a in listing["items"] if self.access.alert_visible(user, a, fleets, services)]
        open_items = [a for a in items if a["resolvedAt"] is None]
        return {"items": items, "counts": {"open": len(open_items),
                                           "unacked": sum(1 for a in open_items if a["ackedAt"] is None),
                                           "critical": sum(1 for a in open_items if a["severity"] == "critical")}}

    # Overview

    def overview(self, user: User) -> dict:
        now = int(time.time() * 1000)
        today = self.day_start(now)
        robots = self.operations.robots(user)
        online = [r for r in robots if r["connection"] == "online"]
        with self.db.session() as s:
            rows = self._visible_rows(s, user, since=today)
        states = [(r, views.task_state(r)) for r in rows]
        finished_today = [(r, st) for r, st in states if (r.finished_at or 0) >= today]
        completed = sum(1 for _, st in finished_today if st == "COMPLETED")
        failed = sum(1 for _, st in finished_today if st == "FAILED")
        attention = self.visible_alerts(user)
        services = []
        for sid in self._service_ids(user):
            svc = self.access.registry.get(sid)
            members = [r for r in robots if sid in r["services"]]
            own = [(r, st) for r, st in states if (r.service or r.kind) == sid]
            services.append({
                "id": sid, "enabled": self.access.registry.enabled(svc),
                "available": bool(self.access.registry.capable_fleets(svc, self.service.known_fleets())),
                "robots": len(members), "byStatus": _group_counts(members),
                "tasksToday": sum(1 for r, _ in own if r.created_at >= today),
                "activeTasks": sum(1 for _, st in own if st in OPEN_STATES),
                "metrics": self._service_metrics(svc, own, today),
            })
        other = [r for r in robots if not r["services"]]
        if other and self.access.sees_all(user):
            services.append({"id": "other", "enabled": True, "available": True, "robots": len(other),
                             "byStatus": _group_counts(other), "tasksToday": 0, "activeTasks": 0, "metrics": []})
        out = {
            "robots": {"total": len(robots), "online": len(online),
                       "available": sum(1 for r in online if r["status"] == "IDLE"),
                       "avgBattery": round(sum(r["battery"] for r in online) / len(online), 1) if online else None},
            "tasks": {"active": sum(1 for _, st in states if st in OPEN_STATES),
                      "executing": sum(1 for _, st in states if st == "EXECUTING"),
                      "scheduled": sum(1 for _, st in states if st == "SCHEDULED"),
                      "completedToday": completed, "failedToday": failed,
                      "successRate": round(100 * completed / (completed + failed), 1) if completed + failed else None},
            "attention": attention["counts"],
            "alerts": [a for a in attention["items"] if a["resolvedAt"] is None][:ATTENTION_ROWS],
            "services": services,
            "schedule": self.schedule(user, today, today + 86_400_000)["items"],
        }
        if user.role == "admin":
            out["health"] = self.operations.health()
        return out

    def _service_ids(self, user: User) -> list[str]:
        if user.role == "admin":
            return [s.id for s in self.access.registry.services]
        return self.access.services(user)

    @staticmethod
    def _service_metrics(svc, own: list, today: int) -> list[dict]:
        """Figures of the service's own kind, from its tasks of today."""
        done = [r for r, st in own if st == "COMPLETED" and (r.finished_at or 0) >= today]
        out = [{"key": "completedToday", "value": len(done)}]
        if svc.category == "patrol":
            rounds = sum(r.rounds_done for r, _ in own if (r.finished_at or time.time() * 1000) >= today)
            out.append({"key": "roundsToday", "value": rounds})
        return out

    # Tasks

    def tasks(self, user: User, service: str = "", state: str = "", q: str = "", group: str = "all",
              mine: bool = False) -> dict:
        if group not in TASK_GROUPS:
            raise ApiFail(422, "tasks.invalid_group")
        if state and state not in TASK_STATES:
            raise ApiFail(422, "tasks.invalid_state")
        with self.db.session() as s:
            rows = sorted(self._visible_rows(s, user), key=lambda r: -r.created_at)
            if mine:
                rows = [r for r in rows if r.requester_id == user.id]
            people = {u.id: u for u in s.scalars(select(User)).all()}
            groups = {"active": lambda r: r.status in views.ACTIVE, "scheduled": lambda r: r.status == "scheduled",
                      "finished": lambda r: r.status in views.FINAL, "all": lambda r: True}
            in_service = [r for r in rows if not service or (r.service or r.kind) == service]
            counts = {st: 0 for st in TASK_STATES}
            for r in in_service:
                counts[views.task_state(r)] += 1
            needle = q.strip().lower()

            def matches(r: Delivery) -> bool:
                if not needle:
                    return True
                person = people.get(r.requester_id)
                places = [r.pickup_id, r.dropoff_id, *(r.stops or []), *(r.zones or [])]
                params = r.params or {}
                names = [n for p in places for n in (self.site.location(p) or self.site.zone(p) or {}).get("name", {}).values()]
                for item in (self.site.area(str(params.get("area", ""))), self.site.route_of(str(params.get("route", "")))):
                    if item:
                        names.extend(item["name"].values())
                hay = [str(r.id), f"#{r.id}", r.robot or "", r.fleet or "", r.rmf_task_id or "", r.service or "",
                       person.full_name if person else "", person.email if person else "", *places, *names]
                return any(needle in h.lower() for h in hay)

            picked = [r for r in in_service if groups[group](r) and (not state or views.task_state(r) == state)
                      and matches(r)]
            events: dict[int, list] = {}
            if picked:
                for e in s.scalars(select(DeliveryEvent).where(DeliveryEvent.delivery_id.in_([r.id for r in picked]))):
                    events.setdefault(e.delivery_id, []).append(e)
            items = [views.delivery(self.site, r, events.get(r.id, []), people.get(r.requester_id), user, operator=True)
                     | {"rmfTaskId": r.rmf_task_id} for r in picked]
            open_rows = [r for r in rows if r.status not in views.FINAL]
            finished = [r for r in rows if r.status in views.FINAL]
            summary, unassigned, late = self.operations._task_watch(open_rows, finished)
        return {"items": items, "counts": counts,
                "groups": {g: sum(1 for r in in_service if groups[g](r)) for g in TASK_GROUPS},
                "summary": summary, "unassigned": unassigned, "late": late}

    def task(self, user: User, task_id: int) -> dict:
        with self.db.session() as s:
            row = s.get(Delivery, task_id)
            if row is None or not self.access.task_visible(user, row):
                raise ApiFail(404, "delivery.not_found")
            events = s.scalars(select(DeliveryEvent).where(DeliveryEvent.delivery_id == row.id)
                               .order_by(DeliveryEvent.at)).all()
            people = {u.id: u.full_name for u in s.scalars(select(User)).all()}
            dto = views.delivery(self.site, row, events, s.get(User, row.requester_id), user, operator=True)
            dto["rmfTaskId"] = row.rmf_task_id
            dto["activity"] = [{"at": e.at, "type": e.type, "detail": e.detail} for e in events]
            audit = s.scalars(select(AuditLog).where(AuditLog.target == f"{row.service or row.kind}#{row.id}")
                              .order_by(AuditLog.at)).all()
            dto["activity"] += [{"at": a.at, "type": a.action, "detail": people.get(a.actor_id, "")} for a in audit
                                if a.action != "task.create"]
            dto["activity"].sort(key=lambda e: e["at"])
            return dto

    # Schedule

    def schedule(self, user: User, start: int, end: int, service: str = "", robot: str = "", zone: str = "") -> dict:
        if end <= start or end - start > 62 * 86_400_000:
            raise ApiFail(422, "schedule.invalid_range")
        robots = self.operations.robots(user)
        names = {r["name"] for r in robots}
        items = []
        with self.db.session() as s:
            rows = s.scalars(select(Delivery).where(
                ((Delivery.scheduled_at >= start) & (Delivery.scheduled_at < end))
                | ((Delivery.created_at >= start) & (Delivery.created_at < end))
                | ((Delivery.started_at >= start) & (Delivery.started_at < end))
                | (Delivery.status.not_in(views.FINAL)))).all()
            for r in rows:
                if not self.access.task_visible(user, r):
                    continue
                at = r.scheduled_at or r.started_at or r.created_at
                if not start <= at < end:
                    continue
                if (service and (r.service or r.kind) != service) or (robot and r.robot != robot) \
                        or (zone and zone not in (r.zones or [])):
                    continue
                state = views.task_state(r)
                params = r.params or {}
                items.append({"type": "task", "id": f"task-{r.id}", "taskId": r.id, "at": at,
                              "end": r.finished_at or r.eta_at, "service": r.service or r.kind, "robot": r.robot,
                              "zones": list(r.zones or []), "status": SCHEDULE_STATUS[state], "repeat": r.repeat,
                              "pickupId": r.pickup_id, "dropoffId": r.dropoff_id, "areaId": params.get("area"),
                              "routeId": params.get("route")})
        if not service:
            for item in self.maintenance.items(names):
                at = item["windowStart"] or item["dueAt"]
                if at is None or not start <= at < end or (robot and item["robot"] != robot) or item["status"] == "cancelled":
                    continue
                status = {"done": "completed", "in_progress": "in_progress"}.get(item["status"], "scheduled")
                items.append({"type": "maintenance", "id": f"maintenance-{item['id']}", "maintenanceId": item["id"],
                              "at": at, "end": item["windowEnd"], "robot": item["robot"], "title": item["title"],
                              "status": status, "service": None, "zones": []})
            for run in self.history.charging_sessions(start, end):
                if run["robot"] not in names or (robot and run["robot"] != robot):
                    continue
                items.append({"type": "charging", "id": f"charging-{run['robot']}-{run['start']}", "at": run["start"],
                              "end": None if run["open"] else run["end"], "robot": run["robot"],
                              "status": "in_progress" if run["open"] else "completed", "service": None, "zones": []})
        items.sort(key=lambda i: i["at"])
        return {"from": start, "to": end, "items": items}

    # Task activity

    def activity(self, user: User, period: str = "today", service: str = "") -> dict:
        if period not in ACTIVITY_RANGES:
            raise ApiFail(422, "activity.invalid_range")
        unit, count = ACTIVITY_RANGES[period]
        now = int(time.time() * 1000)
        tz = self._tz()
        if unit == "hour":
            first = datetime.datetime.fromtimestamp(self.day_start(now) / 1000, tz)
            edges = [first + datetime.timedelta(hours=h) for h in range(count + 1)]
        else:
            first = datetime.datetime.fromtimestamp(self.day_start(now) / 1000, tz) - datetime.timedelta(days=count - 1)
            edges = [first + datetime.timedelta(days=d) for d in range(count + 1)]
        bounds = [int(e.timestamp() * 1000) for e in edges]
        with self.db.session() as s:
            rows = [r for r in self._visible_rows(s, user, since=bounds[0])
                    if not service or (r.service or r.kind) == service]
        buckets = []
        for i in range(count):
            lo, hi = bounds[i], bounds[i + 1]
            buckets.append({
                "start": lo,
                "created": sum(1 for r in rows if lo <= r.created_at < hi),
                "running": sum(1 for r in rows if r.started_at and r.started_at < hi and (r.finished_at or now) >= lo
                               and lo <= now),
                "completed": sum(1 for r in rows if r.status == "completed" and lo <= (r.finished_at or 0) < hi),
                "failed": sum(1 for r in rows if r.status == "failed" and lo <= (r.finished_at or 0) < hi),
            })
        return {"range": period, "unit": unit, "buckets": buckets}

    # Catalog

    def services(self, user: User) -> list[dict]:
        registry = self.access.registry
        allowed = set(self.access.services(user))
        fleets = self.service.known_fleets()
        out = []
        for svc in registry.services:
            enabled = registry.enabled(svc)
            if user.role != "admin" and (not enabled or svc.id not in allowed):
                continue
            capable = registry.capable_fleets(svc, fleets)
            out.append(svc.dto() | {"enabled": enabled, "allowed": svc.id in allowed, "available": bool(capable),
                                    "fleets": capable})
        return out

    def catalog(self, user: User) -> dict:
        zones = self.access.zones(user)
        return {
            "zones": [z | {"enabled": self.access.zone_enabled(z["id"]), "allowed": zones is None or z["id"] in zones}
                      for z in self.site.zones],
            "areas": list(self.site.areas),
            "routes": list(self.site.routes),
        }

    # Integrations

    def integrations(self) -> list[dict]:
        ops = self.operations
        rmf = ops.rmf
        health = {h["key"]: h for h in ops.health() if h["key"] in ("gateway", "rmf", "task_events")}
        received = getattr(rmf, "fleets_received", {}) if rmf else {}
        gateway = getattr(rmf, "gateway", {}) if rmf else {}
        offline_ms = self.settings.get("rmf", "fleet_offline_s") * 1000
        now = time.time() * 1000
        reporting = sorted(f for f, at in received.items() if at and now - at <= offline_ms)
        adapters = ops.metrics.snapshot(time.time())["adapters"]
        logs = self._integration_logs()
        redis = urlparse(self.settings.get("redis", "url"))
        cards = [
            {"id": "open_rmf", "status": health.get("rmf", {}).get("status", "unknown"),
             "lastUpdateAt": max(received.values()) if received else None, "fleets": reporting,
             "config": {"requester": self.settings.get("rmf", "requester"),
                        "fleet_offline_s": self.settings.get("rmf", "fleet_offline_s"),
                        "task_events": bool(gateway.get("task_events"))}},
            {"id": "ros2_gateway", "status": health.get("gateway", {}).get("status", "unknown"),
             "lastUpdateAt": gateway.get("at_ms"), "fleets": reporting,
             "config": {"redis": f"{redis.hostname}:{redis.port or 6379}", "prefix": self.settings.get("redis", "prefix"),
                        "ros_domain": gateway.get("ros_domain_id"), "nav_graph_path": gateway.get("nav_graph_path")}},
            {"id": "vda5050", "status": _worst([_metric_status(a["status"]) for a in adapters]) if adapters else "unknown",
             "lastUpdateAt": _ago_ms(min((a["reported_ago_s"] for a in adapters if a["reported_ago_s"] is not None),
                                         default=None), now),
             "fleets": sorted({a["fleet"] for a in adapters}),
             "config": {a["node"]: a["fleet"] for a in adapters}, "adapters": adapters},
            {"id": "mqtt", "status": _worst(["ok" if a["mqtt"]["connected"] else "critical" for a in adapters
                                             if a["reported_ago_s"] is not None]) if adapters else "unknown",
             "lastUpdateAt": None, "fleets": sorted({a["fleet"] for a in adapters if a["mqtt"]["connected"]}),
             "config": {a["node"]: {"connected": a["mqtt"]["connected"], "connections_lost": a["mqtt"]["connections_lost"]}
                        for a in adapters}},
            {"id": "vendor", "status": "not_configured", "lastUpdateAt": None, "fleets": [], "config": {}},
        ]
        for card in cards:
            card["logs"] = logs.get(card["id"], [])
        return cards

    def _integration_logs(self) -> dict[str, list]:
        listing = self.alerts.list("all")["items"]
        with self.db.session() as s:
            audit = s.scalars(select(AuditLog).order_by(AuditLog.at.desc()).limit(200)).all()
        out: dict[str, list] = {}
        for key, prefixes in LOG_PREFIXES.items():
            entries = [{"at": a["openedAt"], "level": a["severity"], "code": a["code"], "params": a["params"],
                        "closedAt": a["resolvedAt"]} for a in listing if a["code"].startswith(prefixes)]
            entries += [{"at": a.at, "level": "info", "code": a.action, "params": {"target": a.target}, "closedAt": None}
                        for a in audit if a.action.startswith(prefixes)]
            out[key] = sorted(entries, key=lambda e: -e["at"])[:INTEGRATION_LOGS]
        return out


def _group_counts(robots: list[dict]) -> dict[str, int]:
    out = {g: 0 for g in dict.fromkeys(ROBOT_GROUPS.values())}
    for r in robots:
        out[ROBOT_GROUPS[r["status"]]] += 1
    return out


def _metric_status(status: str) -> str:
    return {"ok": "ok", "warning": "warning", "critical": "critical", "silent": "critical",
            "absent": "critical"}.get(status, "unknown")


def _worst(statuses: list[str]) -> str:
    for level in ("critical", "warning", "ok"):
        if level in statuses:
            return level
    return "unknown"


def _ago_ms(seconds, now_ms: float) -> int | None:
    return int(now_ms - seconds * 1000) if seconds is not None else None
