"""Operations alerts: conditions of the fleet, the RMF link and the tasks, kept in the database.

A condition alert stays open while its condition holds and closes by itself when it ends. An event alert (a failed
task) stays open until an operator resolves it. Operators acknowledge alerts to show they are handling them. Texts
are a code and parameters, so they follow the reader's language.
"""

from dataclasses import dataclass, field

from sqlalchemy import select

from .db import Alert, Database, Delivery, User, now_ms
from .service import ApiFail
from .settings import Settings

SEVERITY_RANK = {"critical": 0, "warning": 1, "info": 2}


@dataclass
class Condition:
    severity: str
    code: str
    params: dict = field(default_factory=dict)
    robot: str | None = None
    delivery_id: int | None = None
    kind: str = "condition"


def conditions(settings: Settings, rmf_status: str, robots: dict, waiting: list[Delivery], failed: list[Delivery],
               adapter_items: list[dict], now: int, adapters: list[dict] = (),
               maintenance_due: list[dict] = ()) -> dict[str, Condition]:
    """The alert conditions that hold now, by key."""
    out: dict[str, Condition] = {}
    # A fleet whose robots are all silent is one problem (its adapter or link), not one per robot.
    fleets: dict[str, list[dict]] = {}
    for robot in robots.values():
        fleets.setdefault(robot.get("fleet", ""), []).append(robot)
    silent_fleets = {f for f, members in fleets.items() if members and all(r.get("stale") for r in members)}
    for fleet in sorted(silent_fleets):
        out[f"fleet:{fleet}:offline"] = Condition("critical", "fleet.offline", {"fleet": fleet, "robots": len(fleets[fleet])})
    if rmf_status == "unavailable":
        out["rmf:unavailable"] = Condition("critical", "rmf.unavailable")
    elif rmf_status == "offline" and not silent_fleets:
        out["rmf:offline"] = Condition("warning", "rmf.offline")
    low = settings.get("alerts", "battery_low_pct")
    critical = settings.get("alerts", "battery_critical_pct")
    for name, robot in robots.items():
        mode = robot.get("mode", "idle")
        if robot.get("stale"):
            if robot.get("fleet", "") not in silent_fleets:
                out[f"robot:{name}:offline"] = Condition("critical", "robot.offline", {"robot": name}, name)
            continue
        if mode in ("emergency", "adapter_error"):
            out[f"robot:{name}:{mode}"] = Condition("critical", f"robot.{mode}", {"robot": name}, name)
        battery = robot.get("battery", 100.0)
        if battery < low and mode != "charging":
            out[f"robot:{name}:battery"] = Condition("critical" if battery < critical else "warning", "robot.battery_low",
                                                     {"robot": name, "battery": round(battery)}, name)
    for row in waiting:
        since = max(row.created_at, row.scheduled_at or 0)
        out[f"task:{row.id}:waiting"] = Condition("warning", "task.waiting",
                                                  {"id": row.id, "minutes": int((now - since) / 60000)},
                                                  delivery_id=row.id)
    for row in failed:
        out[f"task:{row.id}:failed"] = Condition("warning", "task.failed", {"id": row.id, "error": row.error or ""},
                                                 row.robot, row.id, kind="event")
    for adapter in adapters:
        # RMF keeps reporting a robot whose AGV went silent; the adapter's report counts the robots it hears.
        counts = adapter.get("robots") or {}
        if adapter.get("status") in ("ok", "warning", "critical") and counts.get("online", 0) < counts.get("registered", 0):
            out[f"adapter:{adapter['node']}:robots_offline"] = Condition(
                "warning", "adapter.robots_offline",
                {"fleet": adapter.get("fleet", ""), "online": counts["online"], "registered": counts["registered"],
                 "robot": counts.get("oldest_state_robot", ""), "age": round(counts.get("state_age_max_s", 0))},
                counts.get("oldest_state_robot") or None)
    for item in maintenance_due:
        out[f"maintenance:{item['id']}:due"] = Condition("warning", "maintenance.due",
                                                         {"robot": item["robot"], "title": item["title"]}, item["robot"])
    for item in adapter_items:
        severity = item.get("severity") if item.get("severity") in SEVERITY_RANK else "warning"
        out[item["key"]] = Condition(severity, "adapter.attention",
                                     {"title": item.get("title", ""), "detail": item.get("detail", "")})
    return out


class Alerts:
    def __init__(self, settings: Settings, db: Database):
        self.settings = settings
        self.db = db
        self.active: set[str] = set()
        # Worst severity of the conditions that hold for each robot, for its health.
        self.robot_levels: dict[str, str] = {}
        # When each condition was first seen in its current run; a condition opens an alert after open_after_s.
        self._since: dict[str, int] = {}

    def tick(self, rmf_status: str, robots: dict, adapter_items: list[dict], adapters: list[dict] = (),
             maintenance_due: list[dict] = ()) -> bool:
        """Open, update and close alerts for the conditions of now; True when the alert list changed."""
        now = now_ms()
        waiting_ms = int(self.settings.get("alerts", "task_waiting_s") * 1000)
        failed_ms = int(self.settings.get("alerts", "failed_window_s") * 1000)
        changed = False
        with self.db.session() as s:
            waiting = [r for r in s.scalars(select(Delivery).where(Delivery.status == "queued")).all()
                       if now - max(r.created_at, r.scheduled_at or 0) > waiting_ms]
            failed = s.scalars(select(Delivery).where(Delivery.status == "failed",
                                                      Delivery.finished_at >= now - failed_ms)).all()
            current = conditions(self.settings, rmf_status, robots, waiting, list(failed), adapter_items, now, adapters,
                                 maintenance_due)
            levels: dict[str, str] = {}
            for cond in current.values():
                if cond.robot and cond.kind == "condition" and SEVERITY_RANK[cond.severity] < SEVERITY_RANK[levels.get(cond.robot, "info")]:
                    levels[cond.robot] = cond.severity
            self.robot_levels = levels
            self._since = {key: self._since.get(key, now) for key in current}
            settle_ms = int(self.settings.get("alerts", "open_after_s") * 1000)
            open_rows = {r.key: r for r in s.scalars(select(Alert).where(Alert.resolved_at.is_(None))).all()}
            event_keys = {c_key for c_key, c in current.items() if c.kind == "event"}
            seen_events = set(s.scalars(select(Alert.key).where(Alert.key.in_(event_keys))).all()) if event_keys else set()

            for key, cond in current.items():
                row = open_rows.get(key)
                if row is None:
                    if cond.kind == "event" and key in seen_events:
                        continue
                    if cond.kind == "condition" and now - self._since[key] < settle_ms:
                        continue
                    s.add(Alert(key=key, kind=cond.kind, severity=cond.severity, code=cond.code, params=cond.params,
                                robot=cond.robot, delivery_id=cond.delivery_id, opened_at=now, updated_at=now))
                    changed = True
                elif row.severity != cond.severity or row.params != cond.params:
                    worse = SEVERITY_RANK[cond.severity] < SEVERITY_RANK.get(row.severity, 2)
                    row.severity, row.params, row.updated_at = cond.severity, cond.params, now
                    if worse:
                        row.acked_at = row.acked_by = None
                    changed = True
            for key, row in open_rows.items():
                if row.kind == "condition" and key not in current:
                    row.resolved_at, row.updated_at = now, now
                    changed = True
            self.active = {k for k, c in current.items() if c.kind == "condition"}
            if changed:
                s.commit()
        return changed

    # API

    def list(self, state: str) -> dict:
        with self.db.session() as s:
            query = select(Alert)
            if state == "open":
                query = query.where(Alert.resolved_at.is_(None))
            rows = s.scalars(query.order_by(Alert.opened_at.desc())
                             .limit(self.settings.get("alerts", "history_limit"))).all()
            names = {u.id: u.full_name for u in s.scalars(select(User)).all()}
            open_rows = s.scalars(select(Alert).where(Alert.resolved_at.is_(None))).all()
        items = [self.dto(r, names) for r in rows]
        if state == "open":
            items.sort(key=lambda a: (a["ackedAt"] is not None, SEVERITY_RANK.get(a["severity"], 2), -a["openedAt"]))
        return {"items": items,
                "counts": {"open": len(open_rows), "unacked": sum(1 for r in open_rows if r.acked_at is None),
                           "critical": sum(1 for r in open_rows if r.severity == "critical")}}

    def dto(self, row: Alert, names: dict) -> dict:
        return {"id": row.id, "key": row.key, "kind": row.kind, "severity": row.severity, "code": row.code,
                "params": row.params or {}, "robot": row.robot, "deliveryId": row.delivery_id,
                "openedAt": row.opened_at, "updatedAt": row.updated_at,
                "ackedAt": row.acked_at, "ackedBy": names.get(row.acked_by) if row.acked_by else None,
                "resolvedAt": row.resolved_at, "resolvedBy": names.get(row.resolved_by) if row.resolved_by else None,
                "active": row.resolved_at is None and (row.kind == "event" or row.key in self.active)}

    def acknowledge(self, user: User, alert_id: int | None) -> int:
        """Acknowledge one open alert, or every open one when alert_id is None; returns how many."""
        with self.db.session() as s:
            query = select(Alert).where(Alert.resolved_at.is_(None), Alert.acked_at.is_(None))
            if alert_id is not None:
                row = s.get(Alert, alert_id)
                if row is None:
                    raise ApiFail(404, "alert.not_found")
                if row.resolved_at is not None:
                    raise ApiFail(409, "alert.resolved")
                query = query.where(Alert.id == alert_id)
            rows = s.scalars(query).all()
            now = now_ms()
            for row in rows:
                row.acked_at, row.acked_by, row.updated_at = now, user.id, now
            self.db.audit(s, user.id, "alert.ack", str(alert_id) if alert_id is not None else "all", str(len(rows)))
            s.commit()
            return len(rows)

    def resolve(self, user: User, alert_id: int) -> None:
        with self.db.session() as s:
            row = s.get(Alert, alert_id)
            if row is None:
                raise ApiFail(404, "alert.not_found")
            if row.resolved_at is not None:
                raise ApiFail(409, "alert.resolved")
            if row.kind == "condition" and row.key in self.active:
                raise ApiFail(409, "alert.still_active")
            now = now_ms()
            row.resolved_at, row.resolved_by, row.updated_at = now, user.id, now
            self.db.audit(s, user.id, "alert.resolve", str(alert_id), row.key)
            s.commit()
