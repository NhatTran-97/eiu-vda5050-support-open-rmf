"""Analytics over a range of days: tasks, outcomes, durations, destinations, utilization, battery and alerts.

Every figure comes from the database (tasks, their events, alerts) or the robot history; nothing is estimated.
"""

import datetime
import zoneinfo
from collections import Counter

from sqlalchemy import select

from .db import Alert, Database, Delivery, DeliveryEvent
from .errors import ApiFail
from .settings import Settings
from .site import Site

RANGES = (1, 7, 30)
TOP_PLACES = 8
BATTERY_BINS = ((0, 20), (20, 40), (40, 60), (60, 80), (80, 101))


def _mean(values: list[float]) -> float | None:
    return round(sum(values) / len(values), 1) if values else None


class Analytics:
    def __init__(self, settings: Settings, site: Site, db: Database, operations, history):
        self.settings = settings
        self.site = site
        self.db = db
        self.operations = operations
        self.history = history

    @staticmethod
    def _service_figures(svc, created: list, finished: list) -> dict:
        """Figures of one service; a figure no robot reports (distance, area, coverage) is null."""
        own_created = [r for r in created if (r.service or r.kind) == svc.id]
        done = [r for r in finished if (r.service or r.kind) == svc.id and r.status == "completed" and r.finished_at]
        durations = [(r.finished_at - (r.started_at or max(r.created_at, r.scheduled_at or 0))) / 60000 for r in done]
        out = {"id": svc.id, "category": svc.category, "created": len(own_created), "completed": len(done),
               "avgDurationMin": _mean(durations)}
        if svc.category == "delivery":
            out["distanceKm"] = None
        elif svc.category == "clean":
            out.update(areaM2=None, coveragePct=None, areas=len({(r.params or {}).get("area") for r in done} - {None}))
        elif svc.category == "patrol":
            rounds = sum(r.rounds_done for r in finished if (r.service or r.kind) == svc.id)
            per_round = [d / max(1, r.rounds) for d, r in zip(durations, done)]
            out.update(rounds=rounds, zonesCovered=len({z for r in done for z in (r.zones or [])}),
                       avgRoundMin=_mean(per_round))
        return out

    def _day(self, ms: int) -> str:
        tz = zoneinfo.ZoneInfo(self.settings.get("site", "time_zone"))
        return datetime.datetime.fromtimestamp(ms / 1000, tz).date().isoformat()

    def report(self, days: int, visible=None, service: str = "", robot: str = "", zone: str = "",
               services: list | None = None) -> dict:
        """Figures of the tasks the viewer sees (`visible(row)`), narrowed to a service, a robot or a zone."""
        if days not in RANGES:
            raise ApiFail(422, "analytics.invalid_range")
        since = self.history.day_start(days)
        keys = self.history.day_keys(days)

        def keep(r: Delivery) -> bool:
            return ((visible is None or visible(r)) and (not service or (r.service or r.kind) == service)
                    and (not robot or r.robot == robot) and (not zone or zone in (r.zones or [])))

        with self.db.session() as s:
            created = [r for r in s.scalars(select(Delivery).where(Delivery.created_at >= since)).all() if keep(r)]
            finished = [r for r in s.scalars(select(Delivery).where(Delivery.finished_at >= since)).all() if keep(r)]
            assigned = {d: at for d, at in s.execute(
                select(DeliveryEvent.delivery_id, DeliveryEvent.at).where(DeliveryEvent.type == "assigned", DeliveryEvent.at >= since))}
            alerts = s.scalars(select(Alert).where(Alert.opened_at >= since)).all()
        if robot:
            alerts = [a for a in alerts if a.robot == robot]

        per_day = {k: {"day": k, "created": 0, "completed": 0, "failed": 0, "cancelled": 0} for k in keys}
        for r in created:
            if (k := self._day(r.created_at)) in per_day:
                per_day[k]["created"] += 1
        for r in finished:
            if r.status in ("completed", "failed", "cancelled") and (k := self._day(r.finished_at)) in per_day:
                per_day[k][r.status] += 1
        outcome = Counter(r.status for r in finished if r.status in ("completed", "failed", "cancelled"))
        decided = outcome["completed"] + outcome["failed"]

        durations, waits = [], []
        for r in finished:
            start = max(r.created_at, r.scheduled_at or 0)
            if r.status == "completed" and r.finished_at:
                durations.append((r.finished_at - start) / 60000)
            if r.id in assigned:
                waits.append(max(0, assigned[r.id] - start) / 60000)

        places = Counter(r.dropoff_id for r in created if r.kind == "delivery")
        for r in created:
            if r.kind == "patrol":
                places.update(r.stops or [])
        top = [{"id": pid, "name": (self.site.location(pid) or {}).get("name", {"vi": pid, "en": pid}), "count": n}
               for pid, n in places.most_common(TOP_PLACES)]

        utilization = self.history.utilization(days)
        busy = sum(d["active"] for d in utilization)
        available = sum(d["active"] + d["idle"] + d["charging"] for d in utilization)

        robots = [r for r in self.operations.robots() if not robot or r["name"] == robot]
        battery = [{"from": lo, "to": min(hi, 100), "robots": sum(1 for r in robots if lo <= r["battery"] < hi)}
                   for lo, hi in BATTERY_BINS]

        alerts_per_day = {k: {"day": k, "critical": 0, "warning": 0, "info": 0} for k in keys}
        for a in alerts:
            if (k := self._day(a.opened_at)) in alerts_per_day and a.severity in alerts_per_day[k]:
                alerts_per_day[k][a.severity] += 1
        by_code = Counter(a.code for a in alerts)

        return {
            "days": days,
            "kpis": {
                "created": len(created), "completed": outcome["completed"], "failed": outcome["failed"],
                "cancelled": outcome["cancelled"],
                "successRate": round(100 * outcome["completed"] / decided, 1) if decided else None,
                "avgDurationMin": _mean(durations), "avgWaitMin": _mean(waits),
                "utilizationPct": round(100 * busy / available, 1) if available else None,
                "alerts": len(alerts),
            },
            "tasksPerDay": list(per_day.values()),
            "topPlaces": top,
            "utilization": utilization,
            "battery": battery,
            "alertsPerDay": list(alerts_per_day.values()),
            "alertCodes": [{"code": c, "count": n} for c, n in by_code.most_common(6)],
            "byService": [self._service_figures(sv, created, finished) for sv in (services or [])
                          if not service or sv.id == service],
            "sampleS": self.settings.get("history", "sample_s"),
        }
