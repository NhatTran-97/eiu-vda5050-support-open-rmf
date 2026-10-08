"""Maintenance of the robots: planned items an admin records (an inspection, a part to replace), their due dates and
service windows. A robot is in maintenance while one of its items is in progress or inside its window; operating
hours since the last finished item come from the robot history."""

from sqlalchemy import select

from .db import Database, MaintenanceItem, User, now_ms
from .errors import ApiFail
from .settings import Settings

STATUSES = ("planned", "in_progress", "done", "cancelled")
OPEN = ("planned", "in_progress")
TITLE_MAX = 200
DAY_MS = 86_400_000


def item_dto(row: MaintenanceItem, names: dict) -> dict:
    return {"id": row.id, "robot": row.robot, "title": row.title, "status": row.status, "dueAt": row.due_at,
            "windowStart": row.window_start, "windowEnd": row.window_end, "note": row.note,
            "createdBy": names.get(row.created_by), "createdAt": row.created_at, "doneAt": row.done_at,
            "doneBy": names.get(row.done_by)}


def in_window(row: MaintenanceItem, now: int) -> bool:
    return row.status == "in_progress" or (row.status == "planned" and row.window_start is not None
                                           and row.window_start <= now < (row.window_end or row.window_start))


class Maintenance:
    def __init__(self, settings: Settings, db: Database, history):
        self.settings = settings
        self.db = db
        self.history = history
        self.active: set[str] = set()
        self.due: list[dict] = []

    def tick(self, now: int | None = None) -> bool:
        """Robots in maintenance now and items past their due date; True when either changed."""
        now = now or now_ms()
        with self.db.session() as s:
            rows = s.scalars(select(MaintenanceItem).where(MaintenanceItem.status.in_(OPEN))).all()
        active = {r.robot for r in rows if in_window(r, now)}
        due = [{"id": r.id, "robot": r.robot, "title": r.title, "dueAt": r.due_at}
               for r in rows if r.due_at is not None and r.due_at <= now and r.status == "planned"]
        changed = active != self.active or due != self.due
        self.active, self.due = active, due
        return changed

    def items(self, robots: set[str] | None = None, open_only: bool = False) -> list[dict]:
        with self.db.session() as s:
            stmt = select(MaintenanceItem).order_by(MaintenanceItem.created_at.desc())
            if open_only:
                stmt = stmt.where(MaintenanceItem.status.in_(OPEN))
            rows = s.scalars(stmt).all()
            names = {u.id: u.full_name for u in s.scalars(select(User)).all()}
        return [item_dto(r, names) for r in rows if robots is None or r.robot in robots]

    def table(self, robots: list[dict]) -> list[dict]:
        """One row per robot: health, maintenance status, the next item, hours since the last finished item."""
        now = now_ms()
        soon = int(self.settings.get("maintenance", "due_soon_days") * DAY_MS)
        names = {r["name"] for r in robots}
        items = self.items(names)
        keep = int(self.settings.get("history", "keep_days")) * DAY_MS
        hours_since: dict[str, float | None] = {}
        for name in names:
            done = [i["doneAt"] for i in items if i["robot"] == name and i["status"] == "done" and i["doneAt"]]
            since = max(done) if done else now - keep
            hours_since[name] = self.history.operating_hours(since).get(name, 0.0)
        out = []
        for robot in robots:
            own = [i for i in items if i["robot"] == robot["name"] and i["status"] in OPEN]
            own.sort(key=lambda i: (i["status"] != "in_progress", i["dueAt"] or i["windowStart"] or 1 << 62))
            nxt = own[0] if own else None
            if robot["name"] in self.active:
                status = "in_progress"
            elif nxt and nxt["dueAt"] is not None and nxt["dueAt"] <= now:
                status = "due"
            elif nxt and nxt["dueAt"] is not None and nxt["dueAt"] - now <= soon:
                status = "due_soon"
            elif nxt:
                status = "scheduled"
            else:
                status = "none"
            out.append({"robot": robot["name"], "serviceType": robot.get("serviceType"), "health": robot.get("health"),
                        "status": status, "next": nxt, "openItems": len(own),
                        "operatingHours": hours_since.get(robot["name"]), "issue": nxt["title"] if nxt else None})
        return out

    def create(self, actor: User, body: dict, known_robots: set[str]) -> dict:
        robot, title = str(body.get("robot", "")), str(body.get("title", "")).strip()
        if robot not in known_robots:
            raise ApiFail(404, "robot.not_found")
        if not title or len(title) > TITLE_MAX:
            raise ApiFail(422, "maintenance.invalid")
        times = {k: body.get(k) for k in ("dueAt", "windowStart", "windowEnd")}
        if any(v is not None and (not isinstance(v, int) or isinstance(v, bool)) for v in times.values()):
            raise ApiFail(422, "maintenance.invalid_time")
        if times["windowStart"] is not None and (times["windowEnd"] is None or times["windowEnd"] <= times["windowStart"]):
            raise ApiFail(422, "maintenance.invalid_window")
        with self.db.session() as s:
            row = MaintenanceItem(robot=robot, title=title, due_at=times["dueAt"], window_start=times["windowStart"],
                                  window_end=times["windowEnd"], note=str(body.get("note") or "")[:1000],
                                  created_by=actor.id)
            s.add(row)
            self.db.audit(s, actor.id, "maintenance.create", robot, title)
            s.commit()
            names = {actor.id: actor.full_name}
            return item_dto(row, names)

    def update(self, actor: User, item_id: int, body: dict) -> dict:
        with self.db.session() as s:
            row = s.get(MaintenanceItem, item_id)
            if row is None:
                raise ApiFail(404, "maintenance.not_found")
            status = body.get("status")
            if status is not None:
                if status not in STATUSES:
                    raise ApiFail(422, "maintenance.invalid")
                if row.status in ("done", "cancelled"):
                    raise ApiFail(409, "maintenance.closed")
                row.status = status
                if status == "done":
                    row.done_at, row.done_by = now_ms(), actor.id
            for key, column in (("dueAt", "due_at"), ("windowStart", "window_start"), ("windowEnd", "window_end")):
                if key in body:
                    value = body[key]
                    if value is not None and (not isinstance(value, int) or isinstance(value, bool)):
                        raise ApiFail(422, "maintenance.invalid_time")
                    setattr(row, column, value)
            if "note" in body:
                row.note = str(body["note"] or "")[:1000]
            self.db.audit(s, actor.id, "maintenance.update", row.robot, f"#{row.id} {row.status}")
            s.commit()
            names = {u.id: u.full_name for u in s.scalars(select(User)).all()}
            return item_dto(row, names)
