"""One place that decides a task's state from the several sources that report it; plain data, no Qt.

RMF tells the dashboard about a task through the task API responses, the dispatcher's
/dispatch_states, the websocket task events and, indirectly, the robots' task ids in
/fleet_states. They arrive in any order and some of them lag, so each source has a rank:
a better-informed source may change a state in any direction, an equal or weaker one may
only move the task forward (queued, then underway, then finished).
"""

import datetime
import time

FINAL_STATES = ("completed", "failed", "cancelled")
ACTIVE_STATES = ("queued", "underway")

# How well informed each source is about a task's state.
SOURCE_RANK = {
    "local": 0,      # the dashboard's own conclusions, such as a dispatch nobody answered
    "fleet": 1,      # a robot picking up or dropping a task id in /fleet_states
    "dispatch": 2,   # /dispatch_states, the dispatcher's auction and assignment
    "api": 3,        # answers and task states on /task_api_responses
    "events": 3,     # task_state_update over the websocket
}

_PROGRESS = {"queued": 0, "underway": 1, "completed": 2, "failed": 2, "cancelled": 2}


def now_ms() -> int:
    return int(time.time() * 1000)


def epoch_ms(value) -> int | None:
    """Epoch milliseconds from seconds or milliseconds; None when missing or not positive."""
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    if value <= 0:
        return None
    return int(value if value > 1e12 else value * 1000)


def new_task(request_id: str, requester: str, **fields) -> dict:
    """A task record for a request the dashboard is about to send."""
    task = {"id": request_id, "rmf_id": "", "requester": requester, "pickup": "n/a", "robot": "—",
            "destination": "—", "created_ms": now_ms(), "end_ms": None, "end_estimated": False,
            "state": "queued", "state_rank": SOURCE_RANK["local"], "phase": ""}
    task.update(fields)
    return task


def set_state(task: dict, state: str, source: str, at_ms: int | None = None) -> bool:
    """Apply a state reported by `source`; returns whether the task changed.

    A task that finishes gets its real end time; one that comes back from a finished state
    loses it.
    """
    if state not in _PROGRESS:
        return False
    rank = SOURCE_RANK[source]
    current = task.get("state", "queued")
    current_rank = task.get("state_rank", 0)
    if state == current:
        if rank > current_rank:
            task["state_rank"] = rank
        return False
    if rank <= current_rank and _PROGRESS[state] <= _PROGRESS.get(current, 0):
        return False
    task["state"] = state
    task["state_rank"] = rank
    if state in FINAL_STATES:
        if task.get("end_ms") is None or task.get("end_estimated"):
            task["end_ms"] = at_ms if at_ms is not None else now_ms()
            task["end_estimated"] = False
    elif current in FINAL_STATES:
        task["end_ms"] = None
        task["end_estimated"] = False
    return True


def set_estimated_end(task: dict, end_ms: int | None) -> bool:
    """Record RMF's expected finish time of an active task; a second's change or less is ignored."""
    if end_ms is None or task.get("state") in FINAL_STATES:
        return False
    if task.get("end_estimated") and abs((task.get("end_ms") or 0) - end_ms) < 1000:
        return False
    if task.get("end_ms") is not None and not task.get("end_estimated"):
        return False
    task["end_ms"] = end_ms
    task["end_estimated"] = True
    return True


def migrate(task: dict) -> dict:
    """Bring a cached record of an older dashboard to the current fields.

    Older records kept the local date ('23 Sep 2026') and clock times ('10:30:35 PM') as text.
    """
    if "created_ms" in task:
        return task
    created = _parse_local(task.get("date"), task.get("start"))
    end = _parse_local(task.get("date"), task.get("end"))
    if created is not None and end is not None and end < created:
        end += 24 * 3600 * 1000
    task["created_ms"] = created
    task["end_ms"] = end
    task["end_estimated"] = end is not None and task.get("state") not in FINAL_STATES
    task.setdefault("state_rank", SOURCE_RANK["local"])
    for field in ("date", "start", "end"):
        task.pop(field, None)
    return task


def _parse_local(day, clock) -> int | None:
    if not day or not clock or day == "—" or clock == "—":
        return None
    try:
        parsed = datetime.datetime.strptime(f"{day} {clock}", "%d %b %Y %I:%M:%S %p")
    except ValueError:
        return None
    return int(parsed.timestamp() * 1000)
