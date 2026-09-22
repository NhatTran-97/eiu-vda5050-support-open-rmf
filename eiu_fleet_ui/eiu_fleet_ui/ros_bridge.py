"""Bridge RMF fleet and task state between ROS 2 and QML."""

import os
import json
import time
import datetime
import queue
import threading
from pathlib import Path

from PySide6.QtCore import QObject, Qt, Signal, Property, Slot, QTimer

from .config import env_float
from .file_io import DebouncedWriter, user_config_dir

DISPENSER_STATES_TOPIC = "/dispenser_states"
INGESTOR_STATES_TOPIC  = "/ingestor_states"
FLEET_STATES_TOPIC  = "/fleet_states"
TASK_API_TOPIC      = "/task_api_requests"
TASK_RESP_TOPIC     = "/task_api_responses"
DISPATCH_STATES_TOPIC = "/dispatch_states"
LANE_STATES_TOPIC   = "/lane_states"
LANE_CLOSURE_REQUEST_TOPIC = "/lane_closure_requests"
NEGOTIATION_STATUSES_TOPIC = "/rmf_traffic/negotiation_statuses"
# Timings and limits; each can be overridden from the environment.
OFFLINE_AFTER_SEC    = env_float("EIU_RMF_OFFLINE_AFTER", 5.0)
DISPATCH_TIMEOUT_SEC = env_float("EIU_DISPATCH_TIMEOUT", 15.0)   # Unanswered dispatch or cancel fails after this
TASK_CACHE_DELAY_SEC = env_float("EIU_TASK_CACHE_DELAY", 1.0)    # Task table changes are written at most this often
TASK_HISTORY_LIMIT   = int(env_float("EIU_TASK_HISTORY", 50))    # Tasks kept in the table and the cache

TASKS_CACHE_FILE = "tasks_cache.json"
_FINAL_STATES = ("completed", "failed", "cancelled")
REQUESTER = "eiu_fleet_ui"

_MODE_NAMES = {
    0: "IDLE", 1: "CHARGING", 2: "MOVING",  3: "PAUSED",
    4: "WAITING", 5: "EMERGENCY", 6: "GOING_HOME",
    7: "DOCKING", 8: "ERROR", 9: "CLEANING",
}


_STATE_LABEL = {
    "queued":        "queued",
    "selected":      "queued",
    "dispatching":   "queued",
    "uninitialized": "queued",
    "standby":       "queued",
    "underway":      "underway",
    "delayed":       "underway",
    "blocked":       "underway",
    "completed":     "completed",
    "failed":        "failed",
    "error":         "failed",
    "killed":        "failed",
    "cancelled":     "cancelled",
    "canceled":      "cancelled",
    "skipped":       "cancelled",
}

def _fmt_time(ts) -> str:
    """Timestamp -> 'HH:MM:SS AM/PM' (UTC). Returns '' if invalid."""
    if not ts:
        return ""
    ts = float(ts)
    if ts > 1e12:       # Convert milliseconds to seconds
        ts /= 1000.0
    if ts <= 0:
        return ""
    return datetime.datetime.utcfromtimestamp(ts).strftime("%I:%M:%S %p")


def _finish_from_path(path) -> str:
    """Return the UTC finish time from the last RMF path waypoint."""
    if not path:
        return ""
    t_sec = path[-1].t.sec
    if t_sec <= 0:
        return ""
    dt = datetime.datetime.utcfromtimestamp(t_sec)
    return dt.strftime("%I:%M:%S %p")


class RosBridge(QObject):

    robotsChanged    = Signal()
    rmfOnlineChanged = Signal()
    tasksChanged     = Signal()
    pathChanged      = Signal()
    trafficChanged   = Signal()
    workcellsChanged = Signal()
    # request id, kind ("dispatch" or "cancel"), ok, message: answers the request that carries the id.
    dispatchResult   = Signal(str, str, bool, str)
    _rosReady        = Signal()            # Hop back to the GUI thread once ROS entities exist
    _result          = Signal(str, str, bool, str)

    def __init__(self, parent=None):
        super().__init__(parent)

        self._robots_json  = "[]"
        self._rmf_online   = False
        self._last_rx      = 0.0
        self._cache_path   = user_config_dir() / TASKS_CACHE_FILE
        self._cache_writer = DebouncedWriter(self._cache_path, TASK_CACHE_DELAY_SEC, name="tasks-cache-writer")
        self._tasks        = self._load_tasks(self._cache_path)   # Task records, newest first
        self._tasks_json   = json.dumps(self._tasks)
        self._planned_dest = ""
        self._waypoints    = []       # Navigation graph waypoints
        self._fleet_names  = set()    # Every real RMF fleet name loaded (multi-fleet aware)
        self._robot_fleet  = {}       # robot name -> its own RMF fleet name
        self._closed_lanes_by_fleet = {}  # fleet name -> its own closed_lanes list
        self._blocked_lanes    = 0    # Closed lane count (union across fleets)
        self._closed_lane_indices_json = "[]"  # Raw closed lane indices for the map
        self._active_conflicts = 0    # Active RMF negotiations


        self._task_lock = threading.RLock()

        # Map UI request IDs to RMF task IDs.
        self._req_to_rmf: dict[str, str] = {}
        # Cancel request id -> (RMF task id, deadline), until RMF answers.
        self._cancel_requests: dict[str, tuple] = {}
        # Every result is delivered on the GUI thread, also when the request failed at once.
        self._result.connect(self.dispatchResult, Qt.ConnectionType.QueuedConnection)

        # Latest dispenser/ingestor state by workcell guid.
        self._workcell_wait: dict[str, dict] = {}
        self._workcells_json = json.dumps({"dispensers": [], "ingestors": []})

        # Per-robot task id, seeded from the reloaded cache (newest first,
        # so the first match per robot wins).
        self._robot_last_task_id: dict[str, str] = {}
        for t in self._tasks:
            robot = t.get("robot", "—")
            if (t.get("state") in ("queued", "underway") and robot != "—"
                    and t.get("rmf_id") and robot not in self._robot_last_task_id):
                self._robot_last_task_id[robot] = t["rmf_id"]

        self._node        = None
        self._task_pub    = None
        self._api_request_type = None
        self._lane_request_pub = None
        self._lane_request_type = None
        self._command_timer = None
        self._command_queue = queue.SimpleQueue()
        self._lane_command_queue = queue.SimpleQueue()
        self._executor    = None
        self._spin_thread = None
        self._ok          = False

        self._watchdog = QTimer(self)
        self._watchdog.setInterval(2000)
        self._watchdog.timeout.connect(self._check_online)
        self._watchdog.timeout.connect(self._check_dispatch_timeouts)
        self._rosReady.connect(self._watchdog.start)

    def set_waypoints(self, wp_list: list):
        """Store waypoints for nearest-waypoint lookup."""
        self._waypoints = wp_list

    def set_fleet_names(self, names: list):
        """Track fleet names for lane closure requests and state updates."""
        self._fleet_names = set(names)

    def set_robot_fleets(self, mapping: dict):
        """Store each robot's RMF fleet for targeted dispatch."""
        self._robot_fleet = dict(mapping)

    def add_robot_fleet(self, name: str, fleet: str):
        """Remember the fleet of a robot registered while the dashboard runs."""
        self._robot_fleet[name] = fleet

    def remove_robot_fleet(self, name: str):
        self._robot_fleet.pop(name, None)

    @staticmethod
    def _load_tasks(path) -> list:
        """Read the task cache; an unreadable or malformed file gives an empty table."""
        try:
            tasks = json.loads(Path(path).read_text())
        except Exception:
            return []
        if not isinstance(tasks, list):
            return []
        tasks = [t for t in tasks if isinstance(t, dict) and isinstance(t.get("id"), str)]
        for t in tasks:
            for field in ("date", "start", "end", "pickup", "destination", "robot"):
                t.setdefault(field, "—")
            # A cancel that was pending when the cache was written has no answer coming any more.
            t.pop("cancel", None)
            t.pop("cancel_id", None)
        return tasks[:TASK_HISTORY_LIMIT]

    def _save_tasks(self):
        self._cache_writer.submit(self._tasks_json)

    def _publish_tasks(self):
        """Serialise the task table, persist it, and notify QML. Call with _task_lock held."""
        self._tasks_json = json.dumps(self._tasks)
        self._save_tasks()
        self.tasksChanged.emit()

    def _nearest_wp_name(self, robots: list, robot_name: str = "") -> str:
        """Find the nearest waypoint to the named robot, falling back to the first robot."""
        if not robots or not self._waypoints:
            return ""
        r = next((x for x in robots if x.get("name") == robot_name), robots[0])
        rx, ry = float(r.get("x", 0)), float(r.get("y", 0))
        best, best_d = "", float("inf")
        for wp in self._waypoints:
            d = (wp["x"] - rx) ** 2 + (wp["y"] - ry) ** 2
            if d < best_d:
                best_d = d
                best = wp["name"]
        return best

    # Start and stop the ROS connection.

    def start(self, on_node_ready=None):
        """Create ROS entities in a background thread to keep QML responsive."""
        threading.Thread(target=self._start_impl, args=(on_node_ready,), daemon=True).start()

    def _start_impl(self, on_node_ready=None):
        # Inherits ROS_DOMAIN_ID from the environment; EIU_ROS_DOMAIN_ID selects a different one.
        override = os.environ.get("EIU_ROS_DOMAIN_ID")
        if override:
            os.environ["ROS_DOMAIN_ID"] = override

        try:
            import rclpy
            from rclpy.qos import (QoSProfile, QoSDurabilityPolicy,
                                   QoSReliabilityPolicy, QoSHistoryPolicy)
            from rclpy.executors import SingleThreadedExecutor
            from rmf_fleet_msgs.msg import FleetState, LaneStates, LaneRequest
            from rmf_task_msgs.msg import ApiRequest, ApiResponse, DispatchStates
            from rmf_traffic_msgs.msg import NegotiationStatuses
            from rmf_dispenser_msgs.msg import DispenserState
            from rmf_ingestor_msgs.msg import IngestorState
        except Exception as e:
            print(f"[ROS] import error ({e}). GUI runs but without RMF data.")
            return

        try:
            if not rclpy.ok():
                rclpy.init(args=None)
            self._node = rclpy.create_node("eiu_fleet_ui_bridge")

            reliable_tl = QoSProfile(
                history=QoSHistoryPolicy.KEEP_LAST, depth=10,
                reliability=QoSReliabilityPolicy.RELIABLE,
                durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
            )
            reliable_volatile = QoSProfile(
                history=QoSHistoryPolicy.KEEP_LAST, depth=1,
                reliability=QoSReliabilityPolicy.RELIABLE,
                durability=QoSDurabilityPolicy.VOLATILE,
            )

            self._task_pub = self._node.create_publisher(
                ApiRequest, TASK_API_TOPIC, reliable_tl)
            self._api_request_type = ApiRequest

            self._lane_request_pub = self._node.create_publisher(
                LaneRequest, LANE_CLOSURE_REQUEST_TOPIC, reliable_tl)
            self._lane_request_type = LaneRequest

            self._node.create_subscription(
                FleetState, FLEET_STATES_TOPIC, self._on_fleet_state, 10)

            # Receive task updates with TRANSIENT_LOCAL durability.
            self._node.create_subscription(
                ApiResponse, TASK_RESP_TOPIC, self._on_task_response, reliable_tl)

            # Receive auction and assignment results with VOLATILE durability.
            self._node.create_subscription(
                DispatchStates, DISPATCH_STATES_TOPIC,
                self._on_dispatch_states, reliable_volatile)

            self._node.create_subscription(
                LaneStates, LANE_STATES_TOPIC, self._on_lane_states, reliable_tl)
            self._node.create_subscription(
                NegotiationStatuses, NEGOTIATION_STATUSES_TOPIC,
                self._on_negotiation_statuses, reliable_tl)

            # Workcell wait countdown for delivery tasks.
            self._node.create_subscription(
                DispenserState, DISPENSER_STATES_TOPIC,
                lambda msg: self._on_workcell_state(msg, "dispensers"), 10)
            self._node.create_subscription(
                IngestorState, INGESTOR_STATES_TOPIC,
                lambda msg: self._on_workcell_state(msg, "ingestors"), 10)

            # Publish queued commands on the ROS thread to keep the UI responsive.
            self._command_timer = self._node.create_timer(
                0.05, self._drain_commands)

            if on_node_ready:
                on_node_ready(self._node)

            self._executor = SingleThreadedExecutor()
            self._executor.add_node(self._node)
            self._ok = True
            self._spin_thread = threading.Thread(target=self._spin, daemon=True)
            self._spin_thread.start()
            self._rosReady.emit()   # QTimer.start() must run on the GUI thread
            print(f"[ROS] bridge online — domain {os.environ.get('ROS_DOMAIN_ID', '(default)')}")
        except Exception as e:
            print(f"[ROS] start error: {e}")

    def _spin(self):
        from rclpy.executors import ExternalShutdownException

        try:
            self._executor.spin()
        except ExternalShutdownException:
            pass   # ROS was shut down from outside, e.g. by Ctrl-C
        except Exception as e:
            if self._ok:
                print(f"[ROS] executor stopped unexpectedly: {e}")

    def _drain_commands(self):
        """Publish queued API and lane requests from the ROS executor thread."""
        if self._task_pub is not None and self._api_request_type is not None:
            while True:
                try:
                    request_id, json_msg = self._command_queue.get_nowait()
                except queue.Empty:
                    break
                try:
                    msg = self._api_request_type()
                    msg.request_id = request_id
                    msg.json_msg = json_msg
                    self._task_pub.publish(msg)
                except Exception as e:
                    print(f"[ROS] publish failed request_id={request_id}: {e}")

        if self._lane_request_pub is not None and self._lane_request_type is not None:
            while True:
                try:
                    close_lanes, open_lanes = self._lane_command_queue.get_nowait()
                except queue.Empty:
                    break
                # Publish each lane request to every fleet on the map.
                for fleet_name in (self._fleet_names or {""}):
                    try:
                        msg = self._lane_request_type()
                        msg.fleet_name = fleet_name
                        msg.close_lanes = close_lanes
                        msg.open_lanes = open_lanes
                        self._lane_request_pub.publish(msg)
                    except Exception as e:
                        print(f"[ROS] lane request publish failed (fleet={fleet_name}): {e}")

    @Slot()
    def shutdown(self):
        """Release ROS entities in a background thread."""
        self._cache_writer.flush()
        if not self._ok and self._node is None:
            return
        self._ok = False
        self._watchdog.stop()

        node, executor, spin_thread = self._node, self._executor, self._spin_thread
        self._executor = None
        self._spin_thread = None
        self._node = None
        self._task_pub = None
        self._api_request_type = None
        threading.Thread(target=self._shutdown_impl, args=(node, executor, spin_thread),
                          daemon=True).start()

    @staticmethod
    def _shutdown_impl(node, executor, spin_thread):
        try:
            import rclpy
            if executor:
                executor.shutdown(timeout_sec=2.0)
            if spin_thread and spin_thread.is_alive():
                spin_thread.join(timeout=2.0)
            if executor and node:
                executor.remove_node(node)
            if node:
                node.destroy_node()
            if rclpy.ok():
                rclpy.shutdown()
        except Exception as e:
            print(f"[ROS] shutdown warning: {e}")

    # Update the fleet view from /fleet_states.

    def _on_fleet_state(self, msg):
        fleet = msg.name
        current = {r["key"]: r for r in json.loads(self._robots_json)}
        current = {k: v for k, v in current.items() if v["fleet"] != fleet}

        robot_task_map: dict[str, str] = {}   # Task ID to robot name

        for r in msg.robots:
            loc = r.location
            key = f"{fleet}/{r.name}"
            mode_str = _MODE_NAMES.get(r.mode.mode, "—")
            if mode_str == "IDLE" and r.task_id:
                mode_str = "WORKING"
            current[key] = {
                "key":     key,
                "name":    r.name,
                "fleet":   fleet,
                "model":   r.model,
                "status":  mode_str,
                "battery": round(float(r.battery_percent), 1),
                "level":   loc.level_name or "L1",
                "task":    r.task_id or "",
                "finish":  self._robot_finish(r.name, r.path),
                "updated": time.strftime("%H:%M:%S"),
                "x":       float(loc.x),
                "y":       float(loc.y),
                "yaw":     float(loc.yaw),
                "path":    [{"x": float(p.x), "y": float(p.y)} for p in r.path],
            }
            if r.task_id:
                robot_task_map[r.task_id] = (r.name, _finish_from_path(r.path))


            prev_task_id = self._robot_last_task_id.get(r.name, "")
            if prev_task_id and prev_task_id != r.task_id:
                self._mark_task_completed(prev_task_id)
            self._robot_last_task_id[r.name] = r.task_id

        self._robots_json = json.dumps(list(current.values()))
        self._last_rx     = time.monotonic()

        if not self._rmf_online:
            self._rmf_online = True
            self.rmfOnlineChanged.emit()
        self.robotsChanged.emit()

        # Match task records to each robot's current task.
        self._sync_tasks_from_fleet(robot_task_map)

    def _mark_task_completed(self, rmf_id: str):
        """Complete a queued/underway task once its robot drops the task ID."""
        with self._task_lock:
            for task in self._tasks:
                if task.get("rmf_id") == rmf_id and task["state"] in ("queued", "underway"):
                    task["state"] = "completed"
                    if task.get("end", "—") == "—":
                        task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                    self._publish_tasks()
                    break

    def _robot_finish(self, robot_name: str, path) -> str:
        """Finish time: taken from the live path while running, falling back to the most recent task."""
        t = _finish_from_path(path)
        if t:
            return t
        with self._task_lock:
            for task in self._tasks:
                if task.get("robot") == robot_name and task.get("end", "—") != "—":
                    return task["end"]
        return ""

    def _check_online(self):
        stale = (time.monotonic() - self._last_rx) > OFFLINE_AFTER_SEC
        if self._rmf_online and stale:
            self._rmf_online = False
            self.rmfOnlineChanged.emit()

    def _check_dispatch_timeouts(self):
        """Fail queued tasks that receive no RMF task ID, and cancels that RMF never answers."""
        now = time.monotonic()
        updated = False
        with self._task_lock:
            for task in self._tasks:
                if task.get("state") != "queued" or task.get("rmf_id"):
                    continue
                dispatched_at = task.get("_dispatched_at")
                if dispatched_at is None or (now - dispatched_at) <= DISPATCH_TIMEOUT_SEC:
                    continue
                task["state"] = "failed"
                task["error"] = "No response from dispatcher"
                if task.get("end", "—") == "—":
                    task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                updated = True
            if updated:
                self._publish_tasks()
            expired = [(rid, rmf_id) for rid, (rmf_id, deadline) in self._cancel_requests.items() if now > deadline]
            # RMF's own task state can settle a cancel before its answer arrives.
            settled = [(rid, t["state"]) for rid, (rmf_id, _) in self._cancel_requests.items()
                       for t in self._tasks if t.get("rmf_id") == rmf_id and t.get("state") in _FINAL_STATES]

        for request_id, state in settled:
            self._finish_cancel(request_id, state == "cancelled", "" if state == "cancelled" else f"Task already {state}")
        for request_id, _ in expired:
            self._finish_cancel(request_id, False, "No answer from the dispatcher")

    def _finish_cancel(self, request_id: str, success: bool, detail: str = ""):
        """Settle a cancel request: RMF confirmed it, refused it, or did not answer."""
        with self._task_lock:
            pending = self._cancel_requests.pop(request_id, None)
            if pending is None:
                return   # Answered already, or not ours
            for task in self._tasks:
                if task.get("cancel_id") != request_id:
                    continue
                task.pop("cancel_id", None)
                if success:
                    task.pop("cancel", None)
                    task["state"] = "cancelled"
                    if task.get("end", "—") == "—":
                        task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                elif task.get("state") in _FINAL_STATES:
                    task.pop("cancel", None)
                else:
                    task["cancel"] = "failed"
                    task["cancel_error"] = detail or "Cancel refused"
                break
            self._publish_tasks()
        self._result.emit(request_id, "cancel", success, "Cancelled" if success else (detail or "Cancel refused"))

    def _on_lane_states(self, msg):
        if self._fleet_names and msg.fleet_name not in self._fleet_names:
            return
        # Show the union of lane closures reported by all fleets.
        self._closed_lanes_by_fleet[msg.fleet_name] = list(msg.closed_lanes)
        union = sorted({i for lanes in self._closed_lanes_by_fleet.values() for i in lanes})
        indices_json = json.dumps(union)
        if len(union) != self._blocked_lanes or indices_json != self._closed_lane_indices_json:
            self._blocked_lanes = len(union)
            self._closed_lane_indices_json = indices_json
            self.trafficChanged.emit()

    def _on_negotiation_statuses(self, msg):
        count = len(msg.negotiations)
        if count != self._active_conflicts:
            self._active_conflicts = count
            self.trafficChanged.emit()

    def _on_workcell_state(self, msg, group: str = "dispensers"):
        """Track the workcells RMF reports and the wait countdown of delivery tasks."""
        busy = bool(msg.request_guid_queue)
        is_new = msg.guid not in self._workcell_wait
        self._workcell_wait[msg.guid] = {
            "busy": busy,
            "seconds_remaining": float(msg.seconds_remaining) if busy else 0.0,
            "group": group,
        }
        if is_new:
            self._workcells_json = json.dumps({
                g: sorted(guid for guid, w in self._workcell_wait.items() if w["group"] == g)
                for g in ("dispensers", "ingestors")})
            self.workcellsChanged.emit()
        updated = False
        with self._task_lock:
            for task in self._tasks:
                if task.get("state") != "underway":
                    continue
                if msg.guid == task.get("pickup_handler"):
                    kind = "pickup"
                elif msg.guid == task.get("dropoff_handler"):
                    kind = "dropoff"
                else:
                    continue
                if busy:
                    new_wait = {"kind": kind, "wait_seconds_remaining": self._workcell_wait[msg.guid]["seconds_remaining"]}
                elif task.get("wait_kind") == kind:
                    new_wait = {"kind": None, "wait_seconds_remaining": None}
                else:
                    continue
                if task.get("wait_kind") != new_wait["kind"] or task.get("wait_seconds_remaining") != new_wait["wait_seconds_remaining"]:
                    task["wait_kind"] = new_wait["kind"]
                    task["wait_seconds_remaining"] = new_wait["wait_seconds_remaining"]
                    updated = True
        if updated:
            self._publish_tasks()

    def _sync_tasks_from_fleet(self, robot_task_map: dict):
        """Attach live robot names and finish times to known RMF tasks."""
        updated = False
        with self._task_lock:
            for task in self._tasks:
                rmf_id = task.get("rmf_id", "")
                if not rmf_id or rmf_id not in robot_task_map:
                    continue

                robot_name, finish = robot_task_map[rmf_id]
                if task["robot"] != robot_name:
                    task["robot"] = robot_name; updated = True
                if task["state"] == "queued":
                    task["state"] = "underway"; updated = True
                if finish and task.get("end", "—") == "—":
                    task["end"] = finish; updated = True

            if updated:
                self._publish_tasks()

    # Handle RMF task API responses.

    def _on_task_response(self, msg):
        """Apply dispatcher responses and task state updates."""
        try:
            data = json.loads(msg.json_msg)
        except Exception:
            return

        with self._task_lock:
            is_cancel_answer = msg.request_id in self._cancel_requests
        if is_cancel_answer and isinstance(data, dict):
            errors = data.get("errors") or []
            detail = "; ".join(str(e.get("detail") or e.get("category") or e) if isinstance(e, dict) else str(e)
                               for e in errors)
            self._finish_cancel(msg.request_id, bool(data.get("success", False)), detail)
            return

        msg_type = data.get("type", "")

        # Read both direct and wrapped RMF responses.
        if not msg_type and isinstance(data.get("state"), dict):
            state = data["state"]
            booking = state.get("booking", {})
            rmf_id = booking.get("id", "")
            if rmf_id:
                req_id = msg.request_id
                success = bool(data.get("success", False))
                self._attach_rmf_id(req_id, rmf_id, success)
            return

        if msg_type == "dispatch_task_response":
            req_id  = msg.request_id          # UI request ID
            rmf_id  = data.get("task_id", "")
            success = data.get("success", False)
            if not rmf_id:
                return
            self._attach_rmf_id(req_id, rmf_id, success)

        elif msg_type in ("task_state_update", "task_update"):
            task_data = data.get("task", data)   # Wrapped task state
            rmf_id = (task_data.get("booking", {}).get("id")
                      or task_data.get("task_id", ""))
            if not rmf_id:
                return
            robot_name = (task_data.get("assigned_to", {}).get("name")
                          or task_data.get("robot_name", ""))
            status_raw = task_data.get("status", "")
            state_label = _STATE_LABEL.get(status_raw, status_raw)

            # Read the estimated finish time.
            finish_str = ""
            estimate = task_data.get("estimate", {})
            ft = estimate.get("finish_time") if estimate else None
            if ft:
                finish_str = _fmt_time(ft)

            updated = False
            with self._task_lock:
                for task in self._tasks:
                    if task.get("rmf_id") == rmf_id:
                        if robot_name and task["robot"] != robot_name:
                            task["robot"] = robot_name; updated = True
                        if state_label and task["state"] != state_label:
                            task["state"] = state_label; updated = True
                        if finish_str and task.get("end", "—") == "—":
                            task["end"] = finish_str; updated = True
                        if state_label in ("completed", "failed", "cancelled") and task.get("end", "—") == "—":
                            task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                            updated = True
                        break
                else:
                    print(f"[ROS] task_state_update rmf_id={rmf_id} not in local list")

                if updated:
                    self._publish_tasks()

        else:
            # Log response types that have no handler.
            if msg_type:
                print(f"[ROS] task_api_responses type={msg_type!r} (ignored)")

    def _attach_rmf_id(self, req_id: str, rmf_id: str, success: bool):
        """Link the UI request id to the task id allocated by RMF."""
        self._req_to_rmf[req_id] = rmf_id
        print(f"[ROS] task mapped: {req_id} → {rmf_id} (success={success})")
        with self._task_lock:
            for task in self._tasks:
                if task["id"] != req_id:
                    continue
                task["rmf_id"] = rmf_id
                if not success:
                    task["state"] = "failed"
                    if task.get("end", "—") == "—":
                        task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                self._publish_tasks()
                break

    def _on_dispatch_states(self, msg):
        """Reflect RMF auction/assignment results in the local task table."""
        updated = False
        with self._task_lock:
            by_rmf_id = {t.get("rmf_id", ""): t for t in self._tasks
                         if t.get("rmf_id", "")}

            for state in list(msg.active) + list(msg.finished):
                task = by_rmf_id.get(state.task_id)
                if task is None:
                    continue

                # Ignore dispatched snapshots; newer task state may already exist.
                label = None
                if state.status in (1, 2):
                    label = "queued"
                elif state.status == 4:
                    label = "failed"
                elif state.status == 5:
                    label = "cancelled"
                elif state.status != 3:
                    continue

                if label is not None and task["state"] != label:
                    task["state"] = label
                    updated = True

                assignment = state.assignment
                robot_name = assignment.expected_robot_name if assignment.is_assigned else ""
                if robot_name and task.get("robot") != robot_name:
                    task["robot"] = robot_name
                    updated = True

                if state.errors:
                    error_text = "; ".join(state.errors)
                    if task.get("error") != error_text:
                        task["error"] = error_text
                        updated = True

                if label in ("failed", "cancelled") and task.get("end", "—") == "—":
                    task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                    updated = True

            if updated:
                self._publish_tasks()

    # Update task state from WebSocket events.

    def apply_task_state_update(self, data: dict):
        """Apply authoritative task status from an RMF WebSocket event."""
        rmf_id = (data.get("booking") or {}).get("id", "")
        if not rmf_id:
            return

        label = _STATE_LABEL.get(data.get("status", ""), "")
        robot_name = (data.get("assigned_to") or {}).get("name", "")
        finish_str = _fmt_time(data.get("unix_millis_finish_time"))

        errors = (data.get("dispatch") or {}).get("errors") or []
        error_text = "; ".join(
            e.get("detail") or e.get("category", "") for e in errors if isinstance(e, dict))

        # Derive remaining rounds from completed phases.
        completed_phases = data.get("completed") or []
        total_phases = len(completed_phases) + len(data.get("pending") or [])
        active_id = data.get("active")
        if active_id is not None:
            total_phases += 1

        phase_label = ""
        if active_id is not None:
            phase_label = ((data.get("phases") or {}).get(str(active_id)) or {}).get("category") or ""

        updated = False
        with self._task_lock:
            for task in self._tasks:
                if task.get("rmf_id") != rmf_id:
                    continue
                if robot_name and task["robot"] != robot_name:
                    task["robot"] = robot_name; updated = True
                if label and task["state"] != label:
                    task["state"] = label; updated = True
                if finish_str and task.get("end", "—") == "—":
                    task["end"] = finish_str; updated = True
                if error_text and task.get("error") != error_text:
                    task["error"] = error_text; updated = True

                new_phase = "" if label in ("completed", "failed", "cancelled") else phase_label
                if task.get("phase", "") != new_phase:
                    task["phase"] = new_phase; updated = True
                if not new_phase and task.get("wait_kind") is not None:
                    task["wait_kind"] = None
                    task["wait_seconds_remaining"] = None
                    updated = True

                rounds = task.get("rounds", 1)
                if rounds > 1:
                    if label in ("completed", "failed", "cancelled"):
                        remaining = 0
                    elif total_phases > 0:
                        phases_per_round = max(1, total_phases // rounds)
                        remaining = max(0, rounds - len(completed_phases) // phases_per_round)
                    else:
                        remaining = task.get("rounds_remaining", rounds)
                    if task.get("rounds_remaining") != remaining:
                        task["rounds_remaining"] = remaining; updated = True
                break
            if updated:
                self._publish_tasks()

    # Build and submit task requests to RMF.

    @staticmethod
    def _new_request_id(prefix: str = "eiu") -> str:
        import uuid
        return f"{prefix}-{uuid.uuid4().hex[:8]}"

    def _refuse(self, request_id: str, kind: str, message: str) -> str:
        """Report a request that could not be sent; the caller gets the id to match the result."""
        print(f"[ROS] {kind} not sent: {message}")
        self._result.emit(request_id, kind, False, message)
        return request_id

    def _ready(self) -> bool:
        return self._ok and self._task_pub is not None

    def _submit(self, request_id: str, envelope: dict, **fields):
        """Queue a task request for RMF and add its row to the task table."""
        self._command_queue.put((request_id, json.dumps(envelope)))
        record = {
            "id":          request_id,
            "rmf_id":      "",           # Set when RMF returns a task ID
            "date":        time.strftime("%d %b %Y"),
            "requester":   REQUESTER,
            "pickup":      "n/a",
            "robot":       "—",
            "start":       datetime.datetime.now().strftime("%I:%M:%S %p"),
            "end":         "—",
            "state":       "queued",
            "phase":       "",
            "_dispatched_at": time.monotonic(),   # Dispatch timeout start
        }
        record.update(fields)
        with self._task_lock:
            self._tasks.insert(0, record)
            del self._tasks[TASK_HISTORY_LIMIT:]
            self._publish_tasks()

    @Slot(str, str, int, result=str)
    def dispatch(self, category: str, place: str, loops: int) -> str:
        """Submit a task for RMF to assign within the fleet."""
        return self._dispatch(category, place, loops, "")

    @Slot(str, str, int, str, result=str)
    def dispatchToRobot(self, category: str, place: str, loops: int, robot: str) -> str:
        """Submit a task directly to the selected robot."""
        return self._dispatch(category, place, loops, robot)

    @Slot(str, str, str, str, str, int, result=str)
    def dispatchDelivery(self, pickup_place: str, pickup_handler: str,
                         dropoff_place: str, dropoff_handler: str, sku: str, quantity: int) -> str:
        """Delivery task: pick up at a dispenser, drop off at an ingestor."""
        return self._dispatch_delivery(pickup_place, pickup_handler, dropoff_place, dropoff_handler,
                                       sku, quantity, "")

    @Slot(str, str, str, str, str, int, str, result=str)
    def dispatchDeliveryToRobot(self, pickup_place: str, pickup_handler: str, dropoff_place: str,
                                dropoff_handler: str, sku: str, quantity: int, robot: str) -> str:
        """Delivery task submitted directly to the selected robot."""
        return self._dispatch_delivery(pickup_place, pickup_handler, dropoff_place, dropoff_handler,
                                       sku, quantity, robot)

    def _dispatch_delivery(self, pickup_place: str, pickup_handler: str, dropoff_place: str,
                           dropoff_handler: str, sku: str, quantity: int, robot: str) -> str:
        request_id = self._new_request_id()
        if not self._ready():
            return self._refuse(request_id, "dispatch", "ROS not connected — dispatch not sent")
        if robot and not self._robot_registered(robot):
            return self._refuse(request_id, "dispatch",
                                f"{robot} is not registered with RMF (offline) — task not sent")

        payload = {"sku": sku, "quantity": int(quantity)}
        task_request = {
            "category": "delivery",
            "description": {
                "pickup": {"place": pickup_place, "handler": pickup_handler, "payload": payload},
                "dropoff": {"place": dropoff_place, "handler": dropoff_handler, "payload": payload},
            },
            "unix_millis_earliest_start_time": 0,
            "requester": REQUESTER,
        }
        envelope = ({"type": "robot_task_request", "robot": robot,
                     "fleet": self._robot_fleet.get(robot, ""), "request": task_request}
                    if robot else
                    {"type": "dispatch_task_request", "request": task_request})
        self._submit(request_id, envelope,
                     pickup=pickup_place, destination=dropoff_place, robot=robot or "—",
                     rounds=1, rounds_remaining=0,
                     category="delivery", pickup_handler=pickup_handler, dropoff_handler=dropoff_handler)
        print(f"[ROS] delivery dispatch queued → {pickup_place} ({pickup_handler}) -> "
              f"{dropoff_place} ({dropoff_handler}) robot={robot or '(any)'} req_id={request_id}")

        self._planned_dest = dropoff_place
        self.pathChanged.emit()
        self._result.emit(request_id, "dispatch", True, "Sent to the dispatcher")
        return request_id

    @Slot(str)
    def closeLanes(self, indices_json: str):
        """Close lane graph indices (a no-go zone) via RMF's own closure mechanism."""
        try:
            indices = json.loads(indices_json)
        except Exception:
            return
        if indices:
            self._lane_command_queue.put((indices, []))

    @Slot(str)
    def openLanes(self, indices_json: str):
        """Reopen lane graph indices previously closed with closeLanes()."""
        try:
            indices = json.loads(indices_json)
        except Exception:
            return
        if indices:
            self._lane_command_queue.put(([], indices))

    def _robot_registered(self, robot: str) -> bool:
        """True when a fleet adapter has reported this robot to RMF."""
        return any(r["name"] == robot for r in json.loads(self._robots_json))

    def _dispatch(self, category: str, place: str, loops: int, robot: str) -> str:
        request_id = self._new_request_id()
        if not self._ready():
            return self._refuse(request_id, "dispatch", "ROS not connected — dispatch not sent")
        if robot and not self._robot_registered(robot):
            return self._refuse(request_id, "dispatch",
                                f"{robot} is not registered with RMF (offline) — task not sent")

        # Start multi-round patrols at the robot's current waypoint.
        places = [place]
        if loops > 1:
            home = self._nearest_wp_name(json.loads(self._robots_json), robot)
            if home and home != place:
                places = [home, place]

        task_request = {
            "category": category,
            "description": {"places": places, "rounds": int(loops)},
            "unix_millis_earliest_start_time": 0,
            "requester": REQUESTER,
        }
        # Send the task to the selected robot.
        envelope = ({"type": "robot_task_request", "robot": robot,
                     "fleet": self._robot_fleet.get(robot, ""), "request": task_request}
                    if robot else
                    {"type": "dispatch_task_request", "request": task_request})
        self._submit(request_id, envelope, destination=place, robot=robot or "—",
                     rounds=int(loops), rounds_remaining=int(loops), category=category)
        print(f"[ROS] dispatch queued → place={place} loops={loops} "
              f"robot={robot or '(any)'} req_id={request_id}")

        self._planned_dest = place
        self.pathChanged.emit()
        self._result.emit(request_id, "dispatch", True, "Sent to the dispatcher")
        return request_id

    @Slot(str, result=str)
    def cancel_task(self, rmf_id: str) -> str:
        """Ask RMF to cancel a task; the row shows it as cancelling until RMF answers."""
        request_id = self._new_request_id("eiu-cancel")
        if not rmf_id:
            return self._refuse(request_id, "cancel", "The task has no RMF id yet")
        if not self._ready():
            return self._refuse(request_id, "cancel", "ROS not connected — cancel not sent")

        with self._task_lock:
            task = next((t for t in self._tasks if t.get("rmf_id") == rmf_id), None)
            if task is None or task.get("state") not in ("queued", "underway"):
                return self._refuse(request_id, "cancel", "The task is not active")
            if task.get("cancel") == "requested":
                return self._refuse(request_id, "cancel", "A cancel is already waiting for RMF")
            task["cancel"] = "requested"
            task["cancel_id"] = request_id
            task.pop("cancel_error", None)
            self._cancel_requests[request_id] = (rmf_id, time.monotonic() + DISPATCH_TIMEOUT_SEC)
            self._publish_tasks()

        envelope = {"type": "cancel_task_request", "task_id": rmf_id, "requester": REQUESTER, "labels": []}
        self._command_queue.put((request_id, json.dumps(envelope)))
        print(f"[ROS] cancel_task queued → {rmf_id} req_id={request_id}")
        return request_id

    # Properties and signals exposed to QML.

    @Property(bool, notify=rmfOnlineChanged)
    def rmfOnline(self):    return self._rmf_online

    @Property(str, notify=robotsChanged)
    def robotsJson(self):   return self._robots_json

    @Property(str, notify=tasksChanged)
    def tasksJson(self):    return self._tasks_json

    @Property(str, notify=workcellsChanged)
    def workcellsJson(self):   return self._workcells_json

    @Property(int, notify=trafficChanged)
    def blockedLanes(self):    return self._blocked_lanes

    @Property(str, notify=trafficChanged)
    def closedLaneIndicesJson(self): return self._closed_lane_indices_json

    @Property(int, notify=trafficChanged)
    def activeConflicts(self): return self._active_conflicts

    @Property(str, notify=pathChanged)
    def plannedDest(self):  return self._planned_dest
