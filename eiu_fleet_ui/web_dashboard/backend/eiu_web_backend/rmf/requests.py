"""Envelopes for /task_api_requests (rmf_task_msgs/ApiRequest json_msg)."""


def _dispatch(category: str, description: dict, start_ms: int | None, requester: str, priority: str = "normal",
              robot: tuple[str, str] | None = None, labels: list[str] = ()) -> dict:
    """A dispatch_task_request, or a robot_task_request when `robot` is (fleet, robot)."""
    request = {
        "category": category,
        "description": description,
        "unix_millis_earliest_start_time": int(start_ms or 0),
        "requester": requester,
    }
    if priority == "high":
        request["priority"] = {"type": "binary", "value": 1}
    if labels:
        request["labels"] = list(labels)
    if robot:
        return {"type": "robot_task_request", "fleet": robot[0], "robot": robot[1], "request": request}
    return {"type": "dispatch_task_request", "request": request}


def delivery(pickup_place: str, pickup_handler: str, dropoff_place: str, dropoff_handler: str,
             sku: str, start_ms: int | None, requester: str, priority: str = "normal",
             robot: tuple[str, str] | None = None) -> dict:
    """Pick up at a dispenser, drop off at an ingestor."""
    payload = {"sku": sku, "quantity": 1}
    return _dispatch("delivery", {
        "pickup": {"place": pickup_place, "handler": pickup_handler, "payload": payload},
        "dropoff": {"place": dropoff_place, "handler": dropoff_handler, "payload": payload},
    }, start_ms, requester, priority, robot)


def patrol(places: list[str], rounds: int, start_ms: int | None, requester: str, priority: str = "normal",
           robot: tuple[str, str] | None = None) -> dict:
    """Visit the places in order, `rounds` times."""
    return _dispatch("patrol", {"places": list(places), "rounds": int(rounds)}, start_ms, requester, priority, robot)


def clean(zone: str, start_ms: int | None, requester: str, priority: str = "normal",
          robot: tuple[str, str] | None = None, labels: list[str] = ()) -> dict:
    """Clean an RMF clean zone."""
    return _dispatch("clean", {"zone": zone}, start_ms, requester, priority, robot, labels)


def cancel(task_id: str, requester: str) -> dict:
    return {"type": "cancel_task_request", "task_id": task_id, "requester": requester, "labels": []}


def interrupt(task_id: str, requester: str) -> dict:
    """Pause a running task; the answer carries the token that resumes it."""
    return {"type": "interrupt_task_request", "task_id": task_id, "labels": [f"requester={requester}"]}


def resume(task_id: str, tokens: list[str], requester: str) -> dict:
    return {"type": "resume_task_request", "for_task": task_id, "for_tokens": list(tokens),
            "labels": [f"requester={requester}"]}
