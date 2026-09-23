"""What the dashboard shows, computed from the backends' snapshots; plain data, no Qt."""

from dataclasses import dataclass

MOVING_STATUSES = ("MOVING", "DOCKING", "GOING_HOME", "WORKING")
ERROR_STATUSES = ("EMERGENCY", "ERROR")
ACTIVE_TASK_STATES = ("queued", "underway")
PENDING_SYNC = "PENDING SYNC"
NO_RMF_DATA = "NO RMF DATA"


@dataclass(frozen=True)
class Limits:
    """Operator-facing thresholds."""

    low_battery_percent: float = 20.0


# Robots.

def telemetry_summary(tele: dict | None) -> dict | None:
    """The few VDA5050 values a robot row shows, or None without telemetry."""
    if not tele:
        return None
    safety = tele.get("safety") or {}
    fatal = tele.get("fatal_error") or ""
    e_stop = safety.get("e_stop") or "NONE"
    if fatal:
        label = fatal
    elif e_stop != "NONE":
        label = e_stop
    elif safety.get("field_violation"):
        label = "FIELD VIOLATION"
    else:
        label = ""
    return {
        "speed": float(tele.get("speed") or 0.0),
        "stale": tele.get("stale") is True,
        "manual": tele.get("operating_mode") == "MANUAL",
        "not_localized": tele.get("position_initialized") is False,
        "unsafe": bool(safety.get("triggered")) or bool(fatal),
        "safety_label": label,
        "charging": bool(tele.get("charging")),
        "paused": bool(tele.get("paused")),
        "last_rx": float(tele.get("last_rx") or 0.0),
    }


def _rounds(task: dict | None) -> tuple[int, int, int]:
    """(total, remaining, current) of a multi-round patrol; zeros otherwise."""
    if not task or int(task.get("rounds") or 1) <= 1:
        return 0, 0, 0
    total = int(task["rounds"])
    remaining = int(task.get("rounds_remaining") or 0)
    return total, remaining, min(total, max(1, total - remaining + 1))


def display_robots(known: list[tuple[str, str]], rmf_rows: list[dict], telemetry: dict, online: dict,
                   tasks: list[dict], default_fleet: str, stale_fleets=()) -> list[dict]:
    """One row per followed robot: RMF's view where RMF has the robot, a PENDING SYNC placeholder otherwise.

    `known` is (name, fleet) per followed robot, in display order. The VDA5050 battery reading
    replaces RMF's planning value when there is one. A robot of a fleet RMF stopped reporting
    keeps its last known values with the status NO RMF DATA.
    """
    by_name = {row["name"]: row for row in rmf_rows}
    task_by_rmf_id = {}
    for task in tasks:
        if task.get("rmf_id"):
            task_by_rmf_id.setdefault(task["rmf_id"], task)

    out = []
    for name, fleet in known:
        tele = telemetry.get(name)
        rmf = by_name.get(name)
        if rmf is not None:
            row = dict(rmf)
            if tele and tele.get("battery_soc") is not None:
                row["battery"] = tele["battery_soc"] * 100
            row["rmfSynced"] = True
            row["hasBattery"] = True
            if row.get("fleet") in stale_fleets:
                row["status"] = NO_RMF_DATA
        else:
            has_battery = bool(tele and tele.get("battery_soc") is not None)
            own_fleet = fleet or default_fleet
            row = {"key": f"{own_fleet}/{name}", "name": name, "fleet": own_fleet, "model": "",
                   "status": PENDING_SYNC, "battery": tele["battery_soc"] * 100 if has_battery else 0,
                   "level": (tele or {}).get("map_id") or "—", "task": "",
                   "x": 0.0, "y": 0.0, "yaw": 0.0, "path": [], "rmfSynced": False, "hasBattery": has_battery}
        row["online"] = bool(online.get(name))
        row["tele"] = telemetry_summary(tele)
        task = task_by_rmf_id.get(row.get("task") or "")
        row["task_destination"] = task.get("destination", "") if task else ""
        row["rounds_total"], row["rounds_remaining"], row["rounds_current"] = _rounds(task)
        out.append(row)
    return out


def map_robots(rmf_rows: list[dict], online: dict) -> list[dict]:
    """Markers for every robot RMF reports: pose, status and whether its VDA5050 link is up."""
    return [{"key": r["key"], "name": r["name"], "fleet": r["fleet"], "status": r["status"],
             "x": r["x"], "y": r["y"], "yaw": r["yaw"], "online": bool(online.get(r["name"]))}
            for r in rmf_rows]


def routes(rmf_rows: list[dict], telemetry: dict, online: dict, waypoints_by_name: dict,
           destination_of: dict) -> list[dict]:
    """What the map draws for each connected robot: route polylines and the destination it is heading to.

    Polylines follow the VDA5050 order's nodes when the robot reports them ('released' or
    'planned' ahead of the released part), else RMF's planned path. The destination is the
    robot's task destination when it is a graph waypoint, else the end of its path.
    """
    out = []
    for row in rmf_rows:
        name = row["name"]
        if not online.get(name):
            continue
        path = row.get("path") or []
        target = waypoints_by_name.get(destination_of.get(name, ""))
        if target is not None:
            dest = {"x": target["x"], "y": target["y"]}
        elif path:
            dest = {"x": path[-1]["x"], "y": path[-1]["y"]}
        else:
            dest = None

        lines = []
        tail = None
        if path and (row["x"] != 0 or row["y"] != 0):
            start = [row["x"], row["y"]]
            node_states = (telemetry.get(name) or {}).get("node_states") or []
            if node_states:
                prev = start
                for node in node_states:
                    wp = waypoints_by_name.get(node.get("nodeId"))
                    if wp is None:
                        continue
                    style = "released" if node.get("released") else "planned"
                    point = [wp["x"], wp["y"]]
                    if lines and lines[-1]["style"] == style:
                        lines[-1]["points"].append(point)
                    else:
                        lines.append({"style": style, "points": [prev, point]})
                    prev = point
            else:
                lines.append({"style": "released", "points": [start] + [[p["x"], p["y"]] for p in path]})
            before = path[-2] if len(path) >= 2 else {"x": row["x"], "y": row["y"]}
            tail = {"x": before["x"], "y": before["y"]}
        if dest is None and not lines:
            continue
        out.append({"key": row["key"], "lines": lines, "dest": dest, "tail": tail})
    return out


def filter_robots(rows: list[dict], text: str) -> list[dict]:
    query = text.strip().lower()
    if not query:
        return rows
    return [r for r in rows if query in str(r.get("name", "")).lower()]


def fleet_status_summary(rows: list[dict]) -> str:
    """'1 error · 2 navigating · 1 idle · 1 offline'; a robot without VDA5050 connection counts as offline."""
    counts = {"error": 0, "navigating": 0, "charging": 0, "idle": 0, "no RMF data": 0, "offline": 0}
    for row in rows:
        status = row.get("status")
        if not row.get("online"):
            counts["offline"] += 1
        elif status == NO_RMF_DATA:
            counts["no RMF data"] += 1
        elif status in MOVING_STATUSES:
            counts["navigating"] += 1
        elif status == "CHARGING":
            counts["charging"] += 1
        elif status in ERROR_STATUSES:
            counts["error"] += 1
        else:
            counts["idle"] += 1
    parts = [f"{n} {label}" for label, n in counts.items() if n > 0]
    return " · ".join(parts) if parts else "No robots"


# Tasks.

def task_display_state(task: dict) -> str:
    """The State column: a pending or refused cancel replaces the RMF state."""
    if task.get("cancel") == "requested":
        return "cancelling"
    if task.get("cancel") == "failed":
        return "cancel failed"
    return str(task.get("state", ""))


def task_rows(tasks: list[dict]) -> list[dict]:
    """Task records with what the table derives from them."""
    out = []
    for task in tasks:
        row = dict(task)
        row["display_state"] = task_display_state(task)
        row["cancellable"] = task.get("state") in ACTIVE_TASK_STATES and task.get("cancel") != "requested"
        out.append(row)
    return out


def filter_tasks(tasks: list[dict], state_filter: str, text: str) -> list[dict]:
    """Tasks matching the state filter ('All' or a state name) and the search text, underway ones first."""
    rows = tasks
    if state_filter and state_filter != "All":
        wanted = state_filter.lower()
        rows = [t for t in rows if str(t.get("state", "")).lower() == wanted]
    query = text.strip().lower()
    if query:
        rows = [t for t in rows if any(query in str(t.get(field, "")).lower()
                                       for field in ("robot", "destination", "requester"))]
    return [t for t in rows if t.get("state") == "underway"] + [t for t in rows if t.get("state") != "underway"]


def task_counts(tasks: list[dict]) -> dict:
    counts = {"underway": 0, "queued": 0, "completed": 0, "failed": 0}
    for task in tasks:
        if task.get("state") in counts:
            counts[task["state"]] += 1
    return counts


def active_destinations(tasks: list[dict]) -> dict:
    """Waypoint name -> True for every underway task's destination."""
    return {t["destination"]: True for t in tasks if t.get("state") == "underway" and t.get("destination")}


# Needs Attention.

def _item(key, severity, title, detail, robot="", since=0.0, pending=None) -> dict:
    return {"key": key, "severity": severity, "robot": robot, "title": title, "detail": detail,
            "since": since, "pending": pending}


def attention_items(*, rmf_online: bool, mqtt_connected: bool, active_conflicts: int, blocked_lanes: int,
                    broker_clashes: list[dict], adapter_items: list[dict], robots: list[dict], telemetry: dict,
                    tasks: list[dict], name_conflicts: list[dict], pending: list[dict],
                    stale_fleets: list[dict] = (), limits: Limits = Limits()) -> list[dict]:
    """Every active issue, most system-wide first; `key` stays the same while the issue lasts."""
    items = []
    if not rmf_online:
        items.append(_item("rmf:offline", "critical", "RMF connection lost", "Fleet traffic coordination unavailable"))
    for fleet in stale_fleets:
        items.append(_item(f"rmf:stale:{fleet['fleet']}", "critical", f"No fleet state from {fleet['fleet']}",
                           "RMF has not reported this fleet; its robots show their last known state",
                           since=fleet.get("last_rx", 0.0)))
    if not mqtt_connected:
        items.append(_item("mqtt:offline", "critical", "MQTT broker disconnected", "No VDA5050 telemetry from any robot"))
    if active_conflicts > 0:
        items.append(_item("traffic:conflicts", "critical", f"{active_conflicts} traffic conflict(s)",
                           "RMF is negotiating a route conflict"))
    if blocked_lanes > 0:
        items.append(_item("traffic:blocked", "warning", f"{blocked_lanes} lane(s) blocked",
                           "A no-go zone is closing part of the map"))
    if broker_clashes:
        items.append(_item("config:brokers", "warning", "Fleets use different MQTT brokers",
                           " · ".join(f"{b['fleet']} → {b['host']}:{b['port']}" for b in broker_clashes)
                           + " · robot state is read from the first only (EIU_MQTT_HOST picks one)"))
    for i, adapter_item in enumerate(adapter_items):
        item = _item(adapter_item.get("key") or f"adapter:{i}", adapter_item.get("severity", "warning"),
                     adapter_item.get("title", ""), adapter_item.get("detail", ""), robot=adapter_item.get("robot", ""))
        items.append(item)

    none_online = not any(r.get("online") for r in robots)
    for row in robots:
        name = row["name"]
        tele = telemetry.get(name)
        online = bool(row.get("online"))
        if not online:
            last_rx = float((tele or {}).get("last_rx") or 0.0)
            items.append(_item(f"robot:{name}:offline", "critical" if none_online else "warning",
                               f"{name} VDA5050 offline",
                               "No VDA5050 state received" if last_rx else "No VDA5050 state ever received",
                               robot=name, since=last_rx))
        if tele and (tele.get("safety") or {}).get("triggered"):
            e_stop = tele["safety"].get("e_stop")
            items.append(_item(f"robot:{name}:estop", "critical", f"{name} emergency stop",
                               e_stop if e_stop and e_stop != "NONE" else "Field violation", robot=name))
        if tele and tele.get("fatal_error"):
            items.append(_item(f"robot:{name}:fatal", "critical", f"{name} fatal error", tele["fatal_error"], robot=name))
        battery = float(row.get("battery") or 0)
        if 0 < battery < limits.low_battery_percent:
            items.append(_item(f"robot:{name}:battery", "warning", f"{name} battery low",
                               f"{battery:.0f}% remaining", robot=name))
        if tele and tele.get("position_initialized") is False:
            items.append(_item(f"robot:{name}:localization", "warning", f"{name} not localized",
                               "No usable pose for route planning", robot=name))
        if tele and tele.get("stale") is True:
            items.append(_item(f"robot:{name}:stale", "warning", f"{name} telemetry stale",
                               "No recent VDA5050 state update", robot=name))
        if online and tele and tele.get("off_graph"):
            items.append(_item(f"robot:{name}:off_graph", "warning", f"{name} off the navigation graph",
                               f"{float(tele.get('off_graph_m') or 0):.1f} m from the nearest lane · "
                               "RMF may not be able to plan its route", robot=name))
        if online and tele and tele.get("paused"):
            items.append(_item(f"robot:{name}:paused", "warning", f"{name} paused",
                               "RMF will not assign tasks until it is resumed", robot=name))

    failed = sum(1 for t in tasks if t.get("state") == "failed")
    if failed:
        items.append(_item("tasks:failed", "warning", f"{failed} task(s) failed", "See Recent Tasks for details"))
    for clash in name_conflicts:
        items.append(_item(f"config:name:{clash['name']}", "warning", f"Robot name '{clash['name']}' is used twice",
                           f"Fleet {clash['fleet']} ignored; following {clash['followed_fleet']}. Rename one of them."))
    for found in pending:
        removed = found.get("removed_as")
        if removed:
            title = f"Removed robot is online again: {removed['name']} ({found['manufacturer']}/{found['serial']})"
            detail = f"Register it again to restore it in {removed['fleet']}"
        else:
            title = f"New robot detected: {found['manufacturer']}/{found['serial']}"
            detail = (found.get("series") or "Type unknown") + " · not registered in any fleet"
        items.append(_item(f"pending:{found['key']}", "info", title, detail, pending=found))
    return items


def health(items: list[dict], rmf_online: bool, mqtt_connected: bool) -> tuple[str, str]:
    """(level, detail): OFFLINE, CRITICAL, DEGRADED or HEALTHY."""
    if not rmf_online and not mqtt_connected:
        return "OFFLINE", "No connection to fleet"
    critical = [i for i in items if i["severity"] == "critical"]
    warning = [i for i in items if i["severity"] == "warning"]
    if critical:
        return "CRITICAL", critical[0]["title"] if len(critical) == 1 else f"{len(critical)} critical issues — see Needs Attention"
    if warning:
        return "DEGRADED", warning[0]["title"] if len(warning) == 1 else f"{len(warning)} warnings — see Needs Attention"
    return "HEALTHY", "All services nominal"


# VDA5050 traffic log.

def filter_traffic(entries: list[dict], type_filter: str) -> list[dict]:
    if not type_filter or type_filter == "all":
        return entries
    return [e for e in entries if e.get("type") == type_filter]
