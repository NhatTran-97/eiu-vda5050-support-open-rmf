"""Deliveries and patrols: create and cancel them, and follow their RMF tasks.

The tracker runs on a fixed tick. It takes the gateway events received since the last tick,
merges the task state of every source with eiu_fleet_ui's task_state ranks, derives the
user-facing status and writes the timeline events and notifications of each change.
"""

import datetime
import logging
import math
import threading
import uuid
import zoneinfo

from sqlalchemy import select
from sqlalchemy.orm import Session

from . import views
from .errors import ApiFail
from .db import Database, Delivery, DeliveryEvent, Notification, User, now_ms
from .rmf import requests
from .rmf.shared import STATE_LABEL, task_state
from .settings import Settings
from .services import REPEATS, FormError
from .site import Site

log = logging.getLogger("eiu_web.service")

# Rank of task events and API answers; a final state reached by a weaker source may still change.
EVENTS_RANK = task_state.SOURCE_RANK["events"]

PROGRESS_NOTIFICATIONS = frozenset({"robot_at_pickup", "package_loaded", "near_destination", "patrol_started"})
DELAY_NOTIFICATIONS = frozenset({"delayed", "task_failed"})

# Status reached -> (timeline event, notification per kind).
TRANSITIONS = {
    "at_pickup": ("arrived_pickup", {"delivery": "robot_at_pickup"}),
    "in_transit": ("picked_up", {"delivery": "package_loaded"}),
    "arrived": ("arrived", {"delivery": "robot_arrived"}),
    "underway": ("started", {"patrol": "patrol_started"}),
    "completed": ("completed", {"delivery": "delivered", "patrol": "patrol_completed"}),
    "cancelled": ("cancelled", {"delivery": "cancelled", "patrol": "cancelled"}),
    "failed": ("failed", {"delivery": "task_failed", "patrol": "task_failed"}),
}


class CommandWaiter:
    """Lets a request thread wait for the gateway's answer to one command, which the tracker delivers.

    A registration request first gets the gateway's "published" result, then the adapter's verdict; only the
    verdict (`final`) ends the wait when the command was registered as `final_only`.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._waiting: dict[str, tuple[threading.Event, dict, bool]] = {}

    def expect(self, cmd_id: str, final_only: bool = False) -> None:
        with self._lock:
            self._waiting[cmd_id] = (threading.Event(), {}, final_only)

    def resolve(self, cmd_id: str, result: dict, final: bool = False) -> None:
        with self._lock:
            entry = self._waiting.get(cmd_id)
            if entry is None:
                return
            event, slot, final_only = entry
            if final_only and not final and result.get("ok"):
                return
            slot.update(result)
            event.set()

    def wait(self, cmd_id: str, timeout_s: float) -> dict | None:
        with self._lock:
            entry = self._waiting.get(cmd_id)
        if entry is None:
            return None
        answered = entry[0].wait(timeout_s)
        with self._lock:
            self._waiting.pop(cmd_id, None)
        return dict(entry[1]) if answered else None


def error_detail(e) -> str:
    if isinstance(e, dict):
        return str(e.get("detail") or e.get("category") or e)
    return str(e)


def derive_status(kind: str, rmf_state: str, phase: int, scheduled_at: int | None, now: int,
                  waiting: str | None, picked: bool) -> str:
    """User-facing status from the merged RMF state and what is known of the delivery's stage."""
    if rmf_state in ("completed", "failed", "cancelled"):
        return rmf_state
    if rmf_state == "queued":
        return "scheduled" if scheduled_at and scheduled_at > now else "queued"
    if kind != "delivery":
        return "underway"
    if waiting == "dropoff":
        return "arrived"
    if picked or phase >= 2:
        return "in_transit"
    if waiting == "pickup":
        return "at_pickup"
    return "to_pickup"


def next_start(at_ms: int, repeat: str, tz) -> int:
    """Start of the next task of a series: the same local time a day or a week later, weekdays skip the weekend."""
    at = datetime.datetime.fromtimestamp(at_ms / 1000, tz)
    at += datetime.timedelta(days=7 if repeat == "weekly" else 1)
    while repeat == "weekdays" and at.weekday() >= 5:
        at += datetime.timedelta(days=1)
    return int(at.timestamp() * 1000)


def path_length(x: float, y: float, path: list[tuple[float, float]]) -> float:
    total, px, py = 0.0, x, y
    for qx, qy in path:
        total += math.hypot(qx - px, qy - py)
        px, py = qx, qy
    return total


class Service:
    def __init__(self, settings: Settings, site: Site, db: Database, rmf, registry=None, access=None):
        self.registry = registry
        self.access = access
        self.settings = settings
        self.site = site
        self.db = db
        self.rmf = rmf
        self._last_task: dict[str, str] = {}
        self._waiting: dict[int, str] = {}
        self._pickup_seen: set[int] = set()
        self._owners: dict[int, str] = {}
        self._task_to_delivery: dict[str, int] = {}
        self._robots: dict = {}
        self._targets: dict[int, tuple[str, str]] = {}
        self.rmf_online = False
        self.waiter = CommandWaiter()
        # Running tasks by id, for the robot views.
        self.live_tasks: dict[int, dict] = {}

    # Status

    def rmf_status(self) -> str:
        if not self.settings.get("rmf", "enabled") or self.rmf is None:
            return "disabled"
        if not getattr(self.rmf, "available", False):
            return "unavailable"
        return "online" if self.rmf_online else "offline"

    # Create and cancel

    def known_fleets(self) -> set[str]:
        """Fleets that report robots now or reported before."""
        return {r["fleet"] for r in self._robots.values()} | set(getattr(self.rmf, "fleets_received", None) or {})

    def create_task(self, s: Session, user: User, body: dict) -> Delivery:
        """A task of a service from {serviceType, parameters, scheduledAt, repeat, robot}."""
        if not isinstance(body, dict):
            raise ApiFail(422, "request.invalid")
        service = self.registry.get(str(body.get("serviceType", "")))
        self.access.check_service(user, service)
        try:
            params, zones = self.registry.validate(service, body.get("parameters") or {}, self.site)
        except FormError as e:
            raise ApiFail(422, e.code, e.key)
        return self._create(s, user, service, params, zones, body)

    def create(self, s: Session, user: User, body: dict) -> Delivery:
        """A task from the request of the first release: {kind: delivery|patrol, pickupId, dropoffId, packageType,
        stops, rounds, note, scheduledAt}."""
        kind = body.get("kind", "delivery")
        if kind not in ("delivery", "patrol"):
            raise ApiFail(422, "task.invalid_kind")
        service = self.registry.by_category(kind)
        self.access.check_service(user, service)
        note = str(body.get("note") or "").strip()
        if kind == "delivery":
            pickup = self.site.location(str(body.get("pickupId", "")))
            dropoff = self.site.location(str(body.get("dropoffId", "")))
            if pickup is None or dropoff is None:
                raise ApiFail(422, "delivery.unknown_location")
            if pickup["id"] == dropoff["id"]:
                raise ApiFail(422, "delivery.same_location")
            item = service.field("itemType")
            package = str(body.get("packageType", "general"))
            if item and package not in item["options"]:
                raise ApiFail(422, "delivery.invalid_package")
            params = {"pickup": pickup["id"], "dropoff": dropoff["id"], "itemType": package, "note": note}
            zones = {pickup["zone"], dropoff["zone"]}
        else:
            stops, rounds = body.get("stops"), body.get("rounds", 1)
            if not isinstance(stops, list) or not stops or len(stops) > self.settings.get("patrol", "max_stops"):
                raise ApiFail(422, "patrol.invalid_stops")
            locs = [self.site.location(str(x)) for x in stops]
            if any(l is None for l in locs):
                raise ApiFail(422, "delivery.unknown_location")
            if any(a["id"] == b["id"] for a, b in zip(locs, locs[1:])):
                raise ApiFail(422, "patrol.repeated_stop")
            if not isinstance(rounds, int) or not 1 <= rounds <= self.settings.get("patrol", "max_rounds"):
                raise ApiFail(422, "patrol.invalid_rounds")
            params = {"stops": [l["id"] for l in locs], "rounds": rounds, "note": note}
            zones = {l["zone"] for l in locs}
        return self._create(s, user, service, params, zones, body)

    def _create(self, s: Session, user: User, service, params: dict, zones: set[str], body: dict,
                previous: Delivery | None = None) -> Delivery:
        self.access.check_scope(user, service, zones, self.known_fleets())
        if self.rmf_status() not in ("online", "offline"):
            raise ApiFail(503, "rmf.unavailable")
        cfg = self.settings
        now = now_ms()
        scheduled_at = body.get("scheduledAt")
        repeat = body.get("repeat") or "none"
        if scheduled_at is not None:
            if service.schedule is None:
                raise ApiFail(422, "task.schedule_not_allowed")
            self.access.require(user, "task.schedule")
            lead = cfg.get("delivery", "schedule_min_lead_s") * 1000
            ahead = cfg.get("delivery", "schedule_max_ahead_days") * 86_400_000
            if not isinstance(scheduled_at, int) or not (now + lead <= scheduled_at <= now + ahead):
                raise ApiFail(422, "delivery.invalid_schedule")
        if repeat != "none" and (repeat not in REPEATS or scheduled_at is None
                                 or not (service.schedule or {}).get("repeat")):
            raise ApiFail(422, "task.invalid_repeat")
        if len(str(params.get("note", ""))) > cfg.get("delivery", "note_max"):
            raise ApiFail(422, "delivery.note_too_long")

        robot = body.get("robot") or None
        if robot is not None:
            live = self._robots.get(str(robot))
            if live is None:
                raise ApiFail(404, "robot.not_found")
            self.access.check_robot(user, service, live["name"], live["fleet"],
                                    "task.reassign" if previous is not None else "fleet.assign")

        if previous is None:
            open_count = len(s.scalars(select(Delivery.id).where(
                Delivery.requester_id == user.id,
                Delivery.status.in_(views.ACTIVE | {"scheduled"}))).all())
            if open_count >= cfg.get("delivery", "max_open"):
                raise ApiFail(409, "delivery.limit_reached")

        row = Delivery(service=service.id, kind=service.category, params=params, zones=sorted(zones),
                       priority=str(params.get("priority", "normal")), repeat=repeat, requested_robot=robot,
                       previous_id=previous.id if previous is not None else None)
        self._fill_places(row)
        row.requester_id = previous.requester_id if previous is not None else user.id
        row.note = str(params.get("note", ""))
        row.created_at = now
        row.scheduled_at = scheduled_at
        row.status = "scheduled" if scheduled_at else "queued"
        row.request_id = f"eiu-web-{uuid.uuid4().hex[:12]}"
        s.add(row)
        s.flush()
        self._event(s, row, "requested", now, robot or "")
        if previous is None:
            self._notify(s, row, "request_received", now)
        self.db.audit(s, user.id, "task.create", f"{row.service}#{row.id}", row.request_id)
        s.commit()
        self._dispatch(s, row, now)
        return row

    def _fill_places(self, row: Delivery) -> None:
        params = row.params or {}
        if row.kind == "delivery":
            row.pickup_id, row.dropoff_id, row.stops, row.rounds = params["pickup"], params["dropoff"], [], 1
            row.package_type = str(params.get("itemType", "general"))
        elif row.kind == "patrol":
            route = self.site.route_of(str(params.get("route", "")))
            stops = list(route["stops"]) if route else list(params.get("stops") or [])
            rounds = int(params.get("rounds", 1))
            if rounds > 1 and len(stops) < 2:
                raise ApiFail(422, "patrol.needs_two_stops")
            row.pickup_id, row.dropoff_id, row.stops, row.rounds = stops[0], stops[-1], stops, rounds
            row.package_type = "general"
        else:
            row.pickup_id = row.dropoff_id = ""
            row.stops, row.rounds, row.package_type = [], 1, "general"

    def envelope(self, row: Delivery) -> dict:
        requester = self.settings.get("rmf", "requester")
        target = None
        if row.requested_robot:
            fleet = (self._robots.get(row.requested_robot) or {}).get("fleet") or row.fleet or ""
            target = (fleet, row.requested_robot)
        if row.kind == "delivery":
            pickup, dropoff = self.site.location(row.pickup_id), self.site.location(row.dropoff_id)
            return requests.delivery(pickup["waypoint"], pickup["dispenser"], dropoff["waypoint"], dropoff["ingestor"],
                                     row.package_type, row.scheduled_at, requester, row.priority, target)
        if row.kind == "patrol":
            places = [self.site.location(x)["waypoint"] for x in row.stops]
            return requests.patrol(places, row.rounds, row.scheduled_at, requester, row.priority, target)
        params = row.params or {}
        area = self.site.area(str(params.get("area", ""))) or {}
        labels = [f"{k}={params[k]}" for k in ("cleaningMode", "duration") if params.get(k) is not None]
        return requests.clean(area.get("rmfZone", ""), row.scheduled_at, requester, row.priority, target, labels)

    def _dispatch(self, s: Session, row: Delivery, now: int) -> None:
        if not self.rmf.send(row.request_id, self.envelope(row)):
            task_state_set(row, "failed", "local", now)
            row.error = "gateway.unreachable"
            self._transition(s, row, "failed", now)
            s.commit()
        log.info("task %s #%d sent as %s", row.service, row.id, row.request_id)

    def cancel(self, s: Session, user: User, row: Delivery, operator: bool = False) -> Delivery:
        dto = views.delivery(self.site, row, [], None, user, operator)
        if not dto["actions"]["cancel"]:
            raise ApiFail(409, "delivery.not_cancellable")
        now = now_ms()
        if not row.rmf_task_id:
            if row.rmf_state == "queued" and row.status in ("queued", "scheduled"):
                raise ApiFail(409, "delivery.not_dispatched_yet")
            raise ApiFail(409, "delivery.not_cancellable")
        row.cancel_request_id = f"eiu-web-cancel-{uuid.uuid4().hex[:10]}"
        row.cancel_deadline = now + int(self.settings.get("rmf", "cancel_timeout_s") * 1000)
        row.error = ""
        self.db.audit(s, user.id, "task.cancel", f"{row.service or row.kind}#{row.id}", row.rmf_task_id)
        s.commit()
        if not self.rmf.send(row.cancel_request_id, requests.cancel(row.rmf_task_id, self.settings.get("rmf", "requester"))):
            row.cancel_request_id = None
            row.cancel_deadline = None
            s.commit()
            raise ApiFail(503, "rmf.unavailable")
        return row

    def pause(self, s: Session, user: User, row: Delivery, resume: bool = False) -> Delivery:
        """Interrupt a running task through RMF, or resume it with the token of the interruption."""
        self.access.require(user, "task.pause")
        dto = views.delivery(self.site, row, [], None, user, True)
        if not dto["actions"]["resume" if resume else "pause"]:
            raise ApiFail(409, "task.not_resumable" if resume else "task.not_pausable")
        requester = self.settings.get("rmf", "requester")
        row.pause_request_id = f"eiu-web-{'resume' if resume else 'interrupt'}-{uuid.uuid4().hex[:10]}"
        envelope = (requests.resume(row.rmf_task_id, [row.pause_token], requester) if resume
                    else requests.interrupt(row.rmf_task_id, requester))
        self.db.audit(s, user.id, "task.resume" if resume else "task.pause", f"{row.service}#{row.id}", row.rmf_task_id)
        s.commit()
        if not self.rmf.send(row.pause_request_id, envelope):
            row.pause_request_id = None
            s.commit()
            raise ApiFail(503, "rmf.unavailable")
        return row

    def reassign(self, s: Session, user: User, row: Delivery, robot: str) -> Delivery:
        """Cancel a task that has not started and send it again to one robot."""
        self.access.require(user, "task.reassign")
        dto = views.delivery(self.site, row, [], None, user, True)
        if not dto["actions"]["reassign"]:
            raise ApiFail(409, "task.not_reassignable")
        service = self.registry.get(row.service or row.kind)
        live = self._robots.get(robot)
        if service is None or live is None:
            raise ApiFail(404, "robot.not_found")
        self.access.check_robot(user, service, robot, live["fleet"], "task.reassign")
        self.cancel(s, user, row, operator=True)
        body = {"scheduledAt": row.scheduled_at if row.scheduled_at and row.scheduled_at > now_ms() else None,
                "repeat": row.repeat, "robot": robot}
        new = self._create(s, user, service, dict(row.params or {}), set(row.zones or []), body, previous=row)
        self._event(s, row, "reassigned", now_ms(), str(new.id))
        s.commit()
        return new

    def _next_occurrence(self, s: Session, row: Delivery, now: int) -> None:
        """Queue the next task of a repeating series once the current one is due."""
        if row.repeat == "none" or not row.scheduled_at:
            return
        if s.scalar(select(Delivery.id).where(Delivery.previous_id == row.id).limit(1)) is not None:
            return
        tz = zoneinfo.ZoneInfo(self.settings.get("site", "time_zone"))
        at = next_start(row.scheduled_at, row.repeat, tz)
        lead = int(self.settings.get("delivery", "schedule_min_lead_s") * 1000)
        while at < now + lead:
            at = next_start(at, row.repeat, tz)
        nxt = Delivery(service=row.service, kind=row.kind, params=dict(row.params or {}), zones=list(row.zones or []),
                       priority=row.priority, repeat=row.repeat, requested_robot=row.requested_robot,
                       previous_id=row.id, requester_id=row.requester_id, pickup_id=row.pickup_id,
                       dropoff_id=row.dropoff_id, stops=list(row.stops or []), rounds=row.rounds,
                       package_type=row.package_type, note=row.note, created_at=now, scheduled_at=at,
                       status="scheduled", request_id=f"eiu-web-{uuid.uuid4().hex[:12]}")
        s.add(nxt)
        s.flush()
        self._event(s, nxt, "requested", now, f"repeat of #{row.id}")
        self._dispatch(s, nxt, now)

    # Tracker

    def tick(self) -> dict[str, set[str]]:
        """Apply what RMF reported since the last tick; returns the topics changed per user id."""
        offline_s = self.settings.get("rmf", "fleet_offline_s")
        events = self.rmf.drain() if self.rmf else []
        robots, online, workcells = self.rmf.snapshot(offline_s) if self.rmf else ({}, False, {})
        self._robots = robots
        self.rmf_online = online
        changed: dict[str, set[str]] = {}
        now = now_ms()

        with self.db.session() as s:
            correctable = now - int(self.settings.get("rmf", "correction_window_s") * 1000)
            rows = s.scalars(select(Delivery).where(
                Delivery.status.not_in(views.FINAL)
                | ((Delivery.finished_at >= correctable) & (Delivery.rmf_state_rank < EVENTS_RANK)))).all()
            by_request = {r.request_id: r for r in rows}
            by_cancel = {r.cancel_request_id: r for r in rows if r.cancel_request_id}
            by_cancel.update({r.pause_request_id: r for r in rows if r.pause_request_id})
            by_task = {r.rmf_task_id: r for r in rows if r.rmf_task_id}
            touched: set[int] = set()

            for event in events:
                if event[0] == "response":
                    self._on_response(event[1], event[2], by_request, by_cancel, by_task, touched)
                elif event[0] == "dispatch":
                    self._on_dispatch(event[1], by_task, touched, s, now)
                elif event[0] == "task_state":
                    self._on_task_state(event[1], by_task, touched, s, now)
                elif event[0] == "command_result":
                    _, cmd_id, ok, error, message = event
                    if not ok:
                        self._on_command_failed(cmd_id, error, by_request, by_cancel, touched, now)
                    self.waiter.resolve(cmd_id, {"ok": ok, "error": error, "message": message})
                elif event[0] == "registration_result":
                    self.waiter.resolve(event[1], {"ok": True, "result": event[2]}, final=True)

            for name, robot in robots.items():
                previous = self._last_task.get(name, "")
                if previous and previous != robot["task_id"] and previous in by_task:
                    row = by_task[previous]
                    if task_state_set(row, "completed", "fleet", now):
                        touched.add(row.id)
                self._last_task[name] = robot["task_id"]
                row = by_task.get(robot["task_id"])
                if row is None:
                    continue
                if task_state_set(row, "underway", "fleet", now):
                    touched.add(row.id)
                if self._assign(s, row, robot["name"], robot["fleet"], now):
                    touched.add(row.id)
                if robot["path_end_ms"] and row.eta_at != robot["path_end_ms"]:
                    row.eta_at = robot["path_end_ms"]

            self._waiting = {}
            for row in rows:
                if row.kind != "delivery" or row.rmf_state != "underway":
                    continue
                pickup = self.site.location(row.pickup_id) or {}
                dropoff = self.site.location(row.dropoff_id) or {}
                if workcells.get(pickup.get("dispenser"), {}).get("busy"):
                    self._waiting[row.id] = "pickup"
                    self._pickup_seen.add(row.id)
                elif workcells.get(dropoff.get("ingestor"), {}).get("busy") and row.status in ("in_transit", "arrived"):
                    self._waiting[row.id] = "dropoff"

            timeout = int(self.settings.get("rmf", "dispatch_timeout_s") * 1000)
            for row in rows:
                if not row.rmf_task_id and row.rmf_state == "queued" and now - row.created_at > timeout:
                    if task_state_set(row, "failed", "local", now):
                        row.error = "dispatch.no_response"
                        touched.add(row.id)
                if row.cancel_request_id and row.cancel_deadline and now > row.cancel_deadline:
                    row.cancel_request_id = None
                    row.cancel_deadline = None
                    row.error = "cancel.no_response"
                    touched.add(row.id)

            arriving_ms = int(self.settings.get("delivery", "arriving_soon_s") * 1000)
            for row in rows:
                picked = row.id in self._pickup_seen and self._waiting.get(row.id) != "pickup"
                status = derive_status(row.kind, row.rmf_state, row.phase, row.scheduled_at, now,
                                       self._waiting.get(row.id), picked)
                if status != row.status:
                    self._transition(s, row, status, now)
                    touched.add(row.id)
                if (row.status == "in_transit" and row.eta_at and row.eta_at - now <= arriving_ms
                        and not self._has_event(s, row, "arriving")):
                    self._event(s, row, "arriving", now)
                    self._notify(s, row, "near_destination", now)
                    touched.add(row.id)
                if row.status in views.FINAL:
                    self._pickup_seen.discard(row.id)

            for row in rows:
                if row.id in touched:
                    changed.setdefault(row.requester_id, set()).update({"deliveries", "notifications"})
            s.commit()

            live = s.scalars(select(Delivery).where(Delivery.status.in_(views.ACTIVE))).all()
            self._owners = {r.id: r.requester_id for r in live}
            self._targets = {}
            for r in live:
                if r.kind != "delivery":
                    continue
                target = self.site.location(r.pickup_id if r.status in ("queued", "to_pickup", "at_pickup") else r.dropoff_id)
                if target:
                    self._targets[r.id] = (target["levelId"], target["waypoint"])
            self._task_to_delivery = {r.rmf_task_id: r.id for r in live if r.rmf_task_id}
            self.live_tasks = {r.id: live_task(r) for r in live}
        return changed

    def robots_live(self) -> list[dict]:
        out = []
        for robot in self._robots.values():
            delivery_id = self._task_to_delivery.get(robot["task_id"])
            level_id = self.site.level_of_rmf(robot["level"])
            path = robot["path"]
            if not path and delivery_id in self._targets and self._targets[delivery_id][0] == level_id:
                level = self.site.levels.get(level_id)
                path = (level.route(robot["x"], robot["y"], self._targets[delivery_id][1]) if level else None) or []
            out.append({
                "name": robot["name"], "fleet": robot["fleet"], "levelId": level_id,
                "x": round(robot["x"], 3), "y": round(robot["y"], 3), "yaw": round(robot["yaw"], 3),
                "battery": round(robot["battery"], 1),
                "activity": "offline" if robot["stale"] else robot["activity"],
                "deliveryId": delivery_id,
                "remainingM": round(path_length(robot["x"], robot["y"], path), 1) if delivery_id and path else None,
                "path": [[round(robot["x"], 3), round(robot["y"], 3)]] + [[round(x, 3), round(y, 3)] for x, y in path] if path else [],
            })
        return out

    def owner_of(self, delivery_id: int | None) -> str | None:
        return self._owners.get(delivery_id) if delivery_id is not None else None

    # Sources

    def _on_response(self, request_id, data, by_request, by_cancel, by_task, touched):
        if not isinstance(data, dict):
            return
        row = by_cancel.get(request_id)
        if row is not None and request_id == row.pause_request_id:
            row.pause_request_id = None
            if data.get("success"):
                row.paused = request_id.startswith("eiu-web-interrupt-")
                row.pause_token = str(data.get("token") or "") if row.paused else ""
                row.error = ""
            else:
                row.error = "; ".join(error_detail(e) for e in data.get("errors") or []) or "pause.refused"
            touched.add(row.id)
            return
        if row is not None:
            row.cancel_request_id = None
            row.cancel_deadline = None
            if data.get("success"):
                task_state_set(row, "cancelled", "api", now_ms())
            else:
                row.error = "; ".join(error_detail(e) for e in data.get("errors") or []) or "cancel.refused"
            touched.add(row.id)
            return
        if data.get("type") in ("task_state_update", "task_update"):
            return
        row = by_request.get(request_id)
        if row is None:
            return
        state = data.get("state") if isinstance(data.get("state"), dict) else {}
        task_id = (state.get("booking") or {}).get("id") or data.get("task_id") or ""
        if task_id and not row.rmf_task_id:
            row.rmf_task_id = task_id
            by_task[task_id] = row
        if not data.get("success", False):
            task_state_set(row, "failed", "api", now_ms())
            row.error = "; ".join(error_detail(e) for e in data.get("errors") or []) or "dispatch.refused"
        touched.add(row.id)

    def _on_command_failed(self, command_id, error, by_request, by_cancel, touched, now):
        """The gateway refused or could not publish a command."""
        row = by_cancel.get(command_id)
        if row is not None and command_id == row.pause_request_id:
            row.pause_request_id = None
            row.error = f"gateway.{error}"
            touched.add(row.id)
            return
        if row is not None:
            row.cancel_request_id = None
            row.cancel_deadline = None
            row.error = f"gateway.{error}"
            touched.add(row.id)
            return
        row = by_request.get(command_id)
        if row is not None and not row.rmf_task_id and task_state_set(row, "failed", "api", now):
            row.error = f"gateway.{error}"
            touched.add(row.id)

    def _on_dispatch(self, states, by_task, touched, s, now):
        for st in states:
            row = by_task.get(st["task_id"])
            if row is None:
                continue
            label = {1: "queued", 2: "queued", 4: "failed", 5: "cancelled"}.get(st["status"])
            if label and task_state_set(row, label, "dispatch", now):
                touched.add(row.id)
            if st["robot"]:
                if self._assign(s, row, st["robot"], st["fleet"], now):
                    touched.add(row.id)
                if row.error:
                    row.error = ""
            elif st["errors"] and label == "failed":
                row.error = "; ".join(st["errors"])

    def _on_task_state(self, data, by_task, touched, s, now):
        task_id = (data.get("booking") or {}).get("id", "")
        row = by_task.get(task_id)
        if row is None:
            return
        label = STATE_LABEL.get(data.get("status", ""), "")
        if label and task_state_set(row, label, "events", now):
            touched.add(row.id)
        assigned = data.get("assigned_to") or {}
        if assigned.get("name") and self._assign(s, row, assigned["name"], assigned.get("group", ""), now):
            touched.add(row.id)
        finish = task_state.epoch_ms(data.get("unix_millis_finish_time"))
        if finish and label not in task_state.FINAL_STATES:
            row.eta_at = finish
        active = data.get("active")
        if isinstance(active, int) and active != row.phase:
            row.phase = active
            touched.add(row.id)
        if row.kind == "patrol" and row.rounds > 1:
            completed = len(data.get("completed") or [])
            total = completed + len(data.get("pending") or []) + (1 if active is not None else 0)
            per_round = max(1, total // row.rounds) if total else 0
            done = row.rounds if label == "completed" else (completed // per_round if per_round else row.rounds_done)
            if done != row.rounds_done:
                row.rounds_done = min(done, row.rounds)
                touched.add(row.id)

    # Records

    def _assign(self, s: Session, row: Delivery, robot: str, fleet: str, now: int) -> bool:
        if row.robot == robot:
            return False
        first = row.robot is None
        row.robot = robot
        row.fleet = fleet or row.fleet
        if first:
            self._event(s, row, "assigned", now, robot)
        return True

    def _transition(self, s: Session, row: Delivery, status: str, now: int) -> None:
        previous = row.status
        row.status = status
        if status in views.ACTIVE and status != "queued" and row.started_at is None:
            row.started_at = now
        if previous == "scheduled" and status in views.ACTIVE:
            self._next_occurrence(s, row, now)
        if status in views.FINAL:
            row.finished_at = now
            row.eta_at = None
            row.paused = False
            if status == "completed" and row.kind == "patrol":
                row.rounds_done = row.rounds
        transition = TRANSITIONS.get(status)
        if transition:
            event_type, notifications = transition
            if not self._has_event(s, row, event_type):
                self._event(s, row, event_type, now)
                if notifications.get(row.kind):
                    self._notify(s, row, notifications[row.kind], now)
        if status == "in_transit" and previous in ("to_pickup", "queued", "scheduled") \
                and not self._has_event(s, row, "arrived_pickup"):
            self._event(s, row, "arrived_pickup", now)
        log.info("task #%d %s -> %s", row.id, previous, status)

    @staticmethod
    def _has_event(s: Session, row: Delivery, event_type: str) -> bool:
        return s.scalar(select(DeliveryEvent.id).where(DeliveryEvent.delivery_id == row.id,
                                                       DeliveryEvent.type == event_type).limit(1)) is not None

    @staticmethod
    def _event(s: Session, row: Delivery, event_type: str, at: int, detail: str = "") -> None:
        s.add(DeliveryEvent(delivery_id=row.id, type=event_type, at=at, detail=detail))

    def _notify(self, s: Session, row: Delivery, kind: str, at: int) -> None:
        user = s.get(User, row.requester_id)
        if user is None:
            return
        if kind in PROGRESS_NOTIFICATIONS and not user.prefs.get("deliveryUpdates", True):
            return
        if kind in DELAY_NOTIFICATIONS and not user.prefs.get("delays", True):
            return
        if row.kind == "clean":
            area = self.site.area(str((row.params or {}).get("area", ""))) or {}
            pickup = dropoff = area.get("name", {"vi": "", "en": ""})
        else:
            pickup = views.location_ref(self.site, row.pickup_id)["name"]
            dropoff = views.location_ref(self.site, row.dropoff_id)["name"]
        s.add(Notification(user_id=user.id, type=kind, delivery_id=row.id, created_at=at, params={
            "id": row.id, "robot": row.robot or "", "pickup": pickup, "dropoff": dropoff, "rounds": row.rounds,
            "service": row.service or row.kind,
        }))


def live_task(row: Delivery) -> dict:
    params = row.params or {}
    return {"id": row.id, "service": row.service or row.kind, "kind": row.kind, "state": views.task_state(row),
            "status": row.status, "pickupId": row.pickup_id, "dropoffId": row.dropoff_id, "stops": list(row.stops or []),
            "areaId": params.get("area"), "routeId": params.get("route"), "rounds": row.rounds,
            "roundsDone": row.rounds_done, "etaAt": row.eta_at, "zones": list(row.zones or [])}


def task_state_set(row: Delivery, state: str, source: str, at_ms: int) -> bool:
    """task_state.set_state applied to the delivery's RMF state columns."""
    record = {"state": row.rmf_state, "state_rank": row.rmf_state_rank, "end_ms": None, "end_estimated": False}
    if not task_state.set_state(record, state, source, at_ms):
        if record["state_rank"] != row.rmf_state_rank:
            row.rmf_state_rank = record["state_rank"]
        return False
    row.rmf_state = record["state"]
    row.rmf_state_rank = record["state_rank"]
    return True
