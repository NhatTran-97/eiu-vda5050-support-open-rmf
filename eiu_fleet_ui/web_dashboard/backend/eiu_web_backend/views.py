"""Database rows to the DTOs of the API contract (frontend/src/api/types.ts)."""

from .db import Delivery, DeliveryEvent, Notification, User
from . import security
from .security import can
from .site import Site

ACTIVE = frozenset({"queued", "to_pickup", "at_pickup", "in_transit", "arrived", "underway"})
FINAL = frozenset({"completed", "cancelled", "failed"})
CANCELLABLE = {
    "delivery": frozenset({"scheduled", "queued", "to_pickup", "at_pickup"}),
    "patrol": frozenset({"scheduled", "queued", "underway"}),
    "clean": frozenset({"scheduled", "queued", "underway"}),
}

# Timeline steps: events that finish a step, and the event that starts it.
STEPS = {
    "delivery": [
        ("requested", (), None),
        ("picked_up", ("picked_up",), "assigned"),
        ("en_route", ("arriving", "arrived"), "picked_up"),
        ("arriving", ("arrived",), "arriving"),
        ("delivered", ("completed",), "arrived"),
    ],
    "patrol": [
        ("requested", (), None),
        ("assigned", ("assigned", "started"), None),
        ("patrolling", ("completed",), "started"),
    ],
    "clean": [
        ("requested", (), None),
        ("assigned", ("assigned", "started"), None),
        ("cleaning", ("completed",), "started"),
    ],
}


def task_state(row: Delivery) -> str:
    """Status of the task model: SCHEDULED, QUEUED, ASSIGNED, EXECUTING, PAUSED, COMPLETED, CANCELLED, FAILED."""
    if row.status in FINAL:
        return row.status.upper()
    if row.status == "scheduled":
        return "SCHEDULED"
    if row.paused:
        return "PAUSED"
    if row.status == "queued":
        return "ASSIGNED" if row.robot else "QUEUED"
    return "EXECUTING"


def group_of(status: str) -> str:
    if status == "scheduled":
        return "upcoming"
    return "active" if status in ACTIVE else "completed"


def me(user: User, access=None) -> dict:
    zones = access.zones(user) if access else None
    return {
        "id": user.id, "email": user.email, "fullName": user.full_name, "role": user.role,
        "department": user.department or "",
        "permissions": security.permissions(user),
        "allowedServices": access.services(user) if access else [],
        "allowedZones": sorted(zones) if zones is not None else [],
        "allZones": zones is None,
        "locale": user.locale,
        "notificationPrefs": {"deliveryUpdates": bool(user.prefs.get("deliveryUpdates", True)),
                              "delays": bool(user.prefs.get("delays", True))},
    }


def location_ref(site: Site, location_id: str) -> dict:
    loc = site.location(location_id)
    if loc is None:
        return {"id": location_id, "name": {"vi": location_id, "en": location_id}, "category": "room", "levelId": ""}
    return {"id": loc["id"], "name": loc["name"], "category": loc["category"], "levelId": loc["levelId"]}


def location(loc: dict) -> dict:
    return {k: loc[k] for k in ("id", "name", "building", "category", "levelId", "waypoint", "x", "y")}


def timeline(row: Delivery, events: list[DeliveryEvent]) -> list[dict]:
    first = {}
    for e in sorted(events, key=lambda e: e.at):
        first.setdefault(e.type, e.at)
    steps, current_taken = [], False
    for key, done_by, started_by in STEPS[row.kind]:
        if key == "requested":
            steps.append({"key": key, "state": "done", "at": row.created_at})
            continue
        done_at = next((first[t] for t in done_by if t in first), None)
        if done_at is not None or row.status == "completed":
            steps.append({"key": key, "state": "done", "at": done_at})
        elif row.status in ("cancelled", "failed"):
            steps.append({"key": key, "state": "skipped", "at": None})
        elif not current_taken and row.status in ACTIVE:
            current_taken = True
            steps.append({"key": key, "state": "current", "at": first.get(started_by) if started_by else None})
        else:
            steps.append({"key": key, "state": "pending", "at": None})
    return steps


def delivery(site: Site, row: Delivery, events: list[DeliveryEvent], requester: User | None, viewer: User,
             operator: bool = False) -> dict:
    """Task DTO; the caller checked that the viewer may see the task."""
    own = row.requester_id == viewer.id
    params = row.params or {}
    state = task_state(row)
    area = site.area(str(params.get("area", ""))) if row.kind == "clean" else None
    route = site.route_of(str(params.get("route", ""))) if params.get("route") else None
    dispatched = bool(row.rmf_task_id) and not row.cancel_request_id and not row.pause_request_id
    return {
        "id": row.id,
        "kind": row.kind,
        "service": row.service or row.kind,
        "state": state,
        "priority": row.priority or "normal",
        "parameters": params,
        "zones": list(row.zones or []),
        "area": {"id": area["id"], "name": area["name"], "zone": area["zone"]} if area else None,
        "route": {"id": route["id"], "name": route["name"], "zone": route["zone"]} if route else None,
        "repeat": row.repeat or "none",
        "previousId": row.previous_id,
        "requestedRobot": row.requested_robot,
        "startedAt": row.started_at,
        "status": row.status,
        "group": group_of(row.status),
        "pickup": location_ref(site, row.pickup_id),
        "dropoff": location_ref(site, row.dropoff_id),
        "stops": [location_ref(site, s) for s in row.stops or []],
        "rounds": row.rounds,
        "roundsDone": row.rounds_done,
        "packageType": row.package_type,
        "note": row.note,
        "requester": {"id": row.requester_id, "fullName": requester.full_name if requester else ""},
        "robot": {"name": row.robot, "fleet": row.fleet or ""} if row.robot else None,
        "createdAt": row.created_at,
        "scheduledAt": row.scheduled_at,
        "finishedAt": row.finished_at,
        "etaAt": row.eta_at if row.status in ACTIVE else None,
        "error": row.error or None,
        "timeline": timeline(row, events),
        "actions": {
            "cancel": row.status in CANCELLABLE[row.kind] and can(viewer, "task.cancel") and not row.cancel_request_id,
            "pause": state == "EXECUTING" and dispatched and can(viewer, "task.pause"),
            "resume": state == "PAUSED" and dispatched and bool(row.pause_token) and can(viewer, "task.pause"),
            "reassign": state in ("SCHEDULED", "QUEUED", "ASSIGNED") and dispatched and can(viewer, "task.reassign"),
            "track": row.status in ACTIVE,
            "reorder": own and row.status in FINAL and can(viewer, "task.create"),
        },
    }


NOTIFICATION_SEVERITY = {"task_failed": "critical", "delayed": "warning", "action_required": "warning",
                         "cancelled": "warning"}


def notification(row: Notification) -> dict:
    return {"id": row.id, "type": row.type, "deliveryId": row.delivery_id, "params": row.params or {},
            "severity": NOTIFICATION_SEVERITY.get(row.type, "info"), "createdAt": row.created_at, "readAt": row.read_at}
