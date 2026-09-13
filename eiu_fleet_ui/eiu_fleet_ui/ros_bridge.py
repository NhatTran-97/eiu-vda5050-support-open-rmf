"""
ROS 2 <-> QML bridge for EIU Fleet UI.

  1. Subscribe  /fleet_states        (rmf_fleet_msgs/FleetState)  -> robot table
  2. Subscribe  /task_api_responses  (rmf_task_msgs/ApiResponse)  -> task state
  3. Subscribe  /dispatch_states    (rmf_task_msgs/DispatchStates) -> assignment state
  4. Publish    /task_api_requests   (rmf_task_msgs/ApiRequest)   -> dispatch task
"""

import os
import json
import time
import datetime
import queue
import threading
from pathlib import Path

from PySide6.QtCore import QObject, Signal, Property, Slot, QTimer

FLEET_STATES_TOPIC  = "/fleet_states"
TASK_API_TOPIC      = "/task_api_requests"
TASK_RESP_TOPIC     = "/task_api_responses"
DISPATCH_STATES_TOPIC = "/dispatch_states"
LANE_STATES_TOPIC   = "/lane_states"
NEGOTIATION_STATUSES_TOPIC = "/rmf_traffic/negotiation_statuses"
OFFLINE_AFTER_SEC   = 5.0
DISPATCH_TIMEOUT_SEC = 15.0   # no dispatch_task_response by then -> mark failed

# Persisted here so a UI restart doesn't lose track of a task the robot is
# still running (see _on_task_response's "not in local list" handling).
_TASKS_CACHE_PATH = Path.home() / ".config" / "eiu_fleet_ui" / "tasks_cache.json"

_MODE_NAMES = {
    0: "IDLE", 1: "CHARGING", 2: "MOVING",  3: "PAUSED",
    4: "WAITING", 5: "EMERGENCY", 6: "GOING_HOME",
    7: "DOCKING", 8: "ERROR", 9: "CLEANING",
}

# RMF task status -> display label. Covers both the dispatcher's own status
# strings and rmf_api_msgs' task_state.json `status` enum (which spells it
# "canceled", one L).
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
    if ts > 1e12:       # milliseconds → seconds
        ts /= 1000.0
    if ts <= 0:
        return ""
    return datetime.datetime.utcfromtimestamp(ts).strftime("%I:%M:%S %p")


def _finish_from_path(path) -> str:
    """Estimated finish time from Location.t of the last waypoint in the RMF path.
    RMF's internal clock starts at the Unix epoch (0), so t.sec=29375 -> 08:09:35 UTC.
    UTC is used so the displayed time of day is correct.
    """
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
    dispatchResult   = Signal(bool, str)   # dispatch/cancel outcome, for dialogs to show

    def __init__(self, parent=None):
        super().__init__(parent)

        self._robots_json  = "[]"
        self._rmf_online   = False
        self._last_rx      = 0.0
        self._tasks        = self._load_tasks()   # list[dict], most recent first
        self._tasks_json   = json.dumps(self._tasks)
        self._planned_dest = ""
        self._waypoints    = []       # list[dict] {name,x,y} from nav_graph
        self._fleet_name   = ""
        self._blocked_lanes    = 0    # rmf_fleet_msgs/LaneStates.closed_lanes count, this fleet
        self._closed_lane_indices_json = "[]"  # same, as raw indices for the map
        self._active_conflicts = 0    # rmf_traffic_msgs/NegotiationStatuses.negotiations


        self._task_lock = threading.RLock()

        # request_id ("eiu-xxx") → rmf internal task_id
        self._req_to_rmf: dict[str, str] = {}

  
        self._robot_last_task_id: dict[str, str] = {}

        self._node        = None
        self._task_pub    = None
        self._api_request_type = None
        self._command_timer = None
        self._command_queue = queue.SimpleQueue()
        self._executor    = None
        self._spin_thread = None
        self._ok          = False

        self._watchdog = QTimer(self)
        self._watchdog.setInterval(2000)
        self._watchdog.timeout.connect(self._check_online)
        self._watchdog.timeout.connect(self._check_dispatch_timeouts)

    def set_waypoints(self, wp_list: list):
        """Receive the waypoint list from MapProvider, for use by _nearest_wp_name."""
        self._waypoints = wp_list

    def set_fleet_name(self, name: str):
        """LaneStates is published per-fleet; only count this fleet's own closures."""
        self._fleet_name = name

    @staticmethod
    def _load_tasks() -> list:
        try:
            return json.loads(_TASKS_CACHE_PATH.read_text())
        except Exception:
            return []

    def _save_tasks(self):
        try:
            _TASKS_CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
            _TASKS_CACHE_PATH.write_text(self._tasks_json)
        except Exception as e:
            print(f"[ROS] failed to persist task cache: {e}")

    def _publish_tasks(self):
        """Serialise the task table, persist it, and notify QML. Call with _task_lock held."""
        self._tasks_json = json.dumps(self._tasks)
        self._save_tasks()
        self.tasksChanged.emit()

    def _nearest_wp_name(self, robots: list, robot_name: str = "") -> str:
        """Find the waypoint nearest to a robot's position.

        Defaults to the first robot when robot_name isn't given or isn't
        found -- only correct for a single-robot fleet, but there's no better
        guess for a fleet-wide dispatch that isn't yet assigned to anyone.
        """
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

    # ── Start / shutdown ──────────────────────────────────────────────────────

    def start(self, on_node_ready=None):
        """`on_node_ready(node)`, if given, runs after the node and this
        bridge's own entities exist but before the executor starts spinning
        -- for callers (e.g. RosControl) that need their own clients/
        publishers on the same node from the first spin iteration.
        """
        # An explicit EIU_ROS_DOMAIN_ID wins; otherwise keep whatever the shell
        # already exported, and only fall back to 42 when nothing is set.
        domain = (os.environ.get("EIU_ROS_DOMAIN_ID")
                  or os.environ.get("ROS_DOMAIN_ID")
                  or "42")
        os.environ["ROS_DOMAIN_ID"] = domain

        try:
            import rclpy
            from rclpy.qos import (QoSProfile, QoSDurabilityPolicy,
                                   QoSReliabilityPolicy, QoSHistoryPolicy)
            from rclpy.executors import SingleThreadedExecutor
            from rmf_fleet_msgs.msg import FleetState, LaneStates
            from rmf_task_msgs.msg import ApiRequest, ApiResponse, DispatchStates
            from rmf_traffic_msgs.msg import NegotiationStatuses
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

            self._node.create_subscription(
                FleetState, FLEET_STATES_TOPIC, self._on_fleet_state, 10)

            # Task state updates: the dispatcher uses TRANSIENT_LOCAL
            self._node.create_subscription(
                ApiResponse, TASK_RESP_TOPIC, self._on_task_response, reliable_tl)

            # Auction/assignment result, including failures such as an unknown
            # waypoint or no fleet bid. Jazzy publishes this as VOLATILE.
            self._node.create_subscription(
                DispatchStates, DISPATCH_STATES_TOPIC,
                self._on_dispatch_states, reliable_volatile)

            self._node.create_subscription(
                LaneStates, LANE_STATES_TOPIC, self._on_lane_states, reliable_tl)
            self._node.create_subscription(
                NegotiationStatuses, NEGOTIATION_STATUSES_TOPIC,
                self._on_negotiation_statuses, reliable_tl)

            # QML queues command payloads; the ROS executor owns publishing so a
            # slow DDS writer can never block Qt's UI thread.
            self._command_timer = self._node.create_timer(
                0.05, self._drain_commands)

            if on_node_ready:
                on_node_ready(self._node)

            self._executor = SingleThreadedExecutor()
            self._executor.add_node(self._node)
            self._ok = True
            self._spin_thread = threading.Thread(target=self._spin, daemon=True)
            self._spin_thread.start()
            self._watchdog.start()
            print(f"[ROS] bridge online — domain {domain}")
        except Exception as e:
            print(f"[ROS] start error: {e}")

    def _spin(self):
        try:
            self._executor.spin()
        except Exception as e:
            if self._ok:
                print(f"[ROS] executor stopped unexpectedly: {e}")

    def _drain_commands(self):
        """Publish queued API requests from the ROS executor thread."""
        if self._task_pub is None or self._api_request_type is None:
            return

        while True:
            try:
                request_id, json_msg = self._command_queue.get_nowait()
            except queue.Empty:
                return

            try:
                msg = self._api_request_type()
                msg.request_id = request_id
                msg.json_msg = json_msg
                self._task_pub.publish(msg)
            except Exception as e:
                print(f"[ROS] publish failed request_id={request_id}: {e}")

    @Slot()
    def shutdown(self):
        if not self._ok and self._node is None:
            return
        self._ok = False
        self._watchdog.stop()

        try:
            import rclpy
            if self._executor:
                self._executor.shutdown(timeout_sec=2.0)
            if self._spin_thread and self._spin_thread.is_alive():
                self._spin_thread.join(timeout=2.0)
            if self._executor and self._node:
                self._executor.remove_node(self._node)
            if self._node:
                self._node.destroy_node()
            if rclpy.ok():
                rclpy.shutdown()
        except Exception as e:
            print(f"[ROS] shutdown warning: {e}")
        finally:
            self._executor = None
            self._spin_thread = None
            self._node = None
            self._task_pub = None
            self._api_request_type = None

    # ── Subscribe: /fleet_states ───────────────────────────────────────────────

    def _on_fleet_state(self, msg):
        fleet = msg.name
        current = {r["key"]: r for r in json.loads(self._robots_json)}
        current = {k: v for k, v in current.items() if v["fleet"] != fleet}

        robot_task_map: dict[str, str] = {}   # rmf_task_id → robot_name

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

            # RMF's task dispatcher never publishes a "completed" event over ROS
            # (further updates go out over a websocket this UI isn't connected to),
            # so a robot's task_id disappearing while "underway" is the only signal we get.
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

        # Cross-reference: update the tasks table based on what each robot is doing
        self._sync_tasks_from_fleet(robot_task_map)

    def _mark_task_completed(self, rmf_id: str):
        """A robot just dropped rmf_id from its /fleet_states task_id.

        Only flips a task that is still "underway": one already marked
        failed/cancelled by an actual signal (dispatch_states, a cancel) is
        left alone, so this can't resurrect or misreport those.
        """
        with self._task_lock:
            for task in self._tasks:
                if task.get("rmf_id") == rmf_id and task["state"] == "underway":
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
        """A queued task with no rmf_id after DISPATCH_TIMEOUT_SEC never got
        a dispatch_task_response -- the dispatcher is down, not just slow.
        """
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

    def _on_lane_states(self, msg):
        if self._fleet_name and msg.fleet_name != self._fleet_name:
            return
        indices = list(msg.closed_lanes)
        indices_json = json.dumps(indices)
        if len(indices) != self._blocked_lanes or indices_json != self._closed_lane_indices_json:
            self._blocked_lanes = len(indices)
            self._closed_lane_indices_json = indices_json
            self.trafficChanged.emit()

    def _on_negotiation_statuses(self, msg):
        count = len(msg.negotiations)
        if count != self._active_conflicts:
            self._active_conflicts = count
            self.trafficChanged.emit()

    def _sync_tasks_from_fleet(self, robot_task_map: dict):
        """Attach the executing robot to tasks we already know the RMF id of.

        FleetState only reports which task_id each robot is running right now. It
        never reports that a task ended, and a task_id missing from it means
        nothing on its own — the robot may be between tasks, paused, or briefly
        unreachable. Terminal states therefore come only from /task_api_responses
        and /dispatch_states, which are authoritative. Nothing is inferred here.
        """
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

    # ── Subscribe: /task_api_responses ────────────────────────────────────────

    def _on_task_response(self, msg):
        """Receive responses from the Jazzy dispatcher: pick up rmf_task_id and state updates."""
        try:
            data = json.loads(msg.json_msg)
        except Exception:
            return

        msg_type = data.get("type", "")

        # Current Jazzy uses an unwrapped response of this form:
        # {"state":{"booking":{"id":"patrol.dispatch-N"}, ...},
        #  "success":true}
        # Keep support for the older wrapped dispatch_task_response schema too.
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
            req_id  = msg.request_id          # "eiu-xxxxxxxx"
            rmf_id  = data.get("task_id", "")
            success = data.get("success", False)
            if not rmf_id:
                return
            self._attach_rmf_id(req_id, rmf_id, success)

        elif msg_type in ("task_state_update", "task_update"):
            task_data = data.get("task", data)   # Jazzy puts state inside "task"
            rmf_id = (task_data.get("booking", {}).get("id")
                      or task_data.get("task_id", ""))
            if not rmf_id:
                return
            robot_name = (task_data.get("assigned_to", {}).get("name")
                          or task_data.get("robot_name", ""))
            status_raw = task_data.get("status", "")
            state_label = _STATE_LABEL.get(status_raw, status_raw)

            # Estimated finish time (may be in seconds or milliseconds)
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
            # Log other types for debugging
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

                # DispatchState constants: queued=1, selected=2, dispatched=3,
                # failed_to_assign=4, canceled_in_flight=5.
                # Status 3 is a stale snapshot the dispatcher keeps re-publishing
                # long after the robot moves on, so it leaves task["state"] untouched.
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

    # ── Websocket: authoritative task state (see task_websocket.py) ───────────

    def apply_task_state_update(self, data: dict):
        """A task_state.json payload from RMF's websocket broadcast -- the
        real completion/failure signal /fleet_states and /task_api_responses
        cannot provide (see _mark_task_completed, still the fallback when the
        adapter has no ui_websocket_uri configured).
        """
        rmf_id = (data.get("booking") or {}).get("id", "")
        if not rmf_id:
            return

        label = _STATE_LABEL.get(data.get("status", ""), "")
        robot_name = (data.get("assigned_to") or {}).get("name", "")
        finish_str = _fmt_time(data.get("unix_millis_finish_time"))

        errors = (data.get("dispatch") or {}).get("errors") or []
        error_text = "; ".join(
            e.get("detail") or e.get("category", "") for e in errors if isinstance(e, dict))

        # Loop/patrol tasks decompose into one phase per leg; the total phase
        # count is stable once known, so dividing it by the round count gives
        # phases-per-round without knowing how the task structures its phases.
        completed_phases = data.get("completed") or []
        total_phases = len(completed_phases) + len(data.get("pending") or [])
        if data.get("active") is not None:
            total_phases += 1

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

    # ── Publish: dispatch task ─────────────────────────────────────────────────

    @Slot(str, str, int)
    def dispatch(self, category: str, place: str, loops: int):
        """Fleet-wide dispatch -- RMF bids the task to whichever robot it picks."""
        self._dispatch(category, place, loops, "")

    @Slot(str, str, int, str)
    def dispatchToRobot(self, category: str, place: str, loops: int, robot: str):
        """Pinned to one robot via RMF's robot_task_request, bypassing bidding."""
        self._dispatch(category, place, loops, robot)

    def _dispatch(self, category: str, place: str, loops: int, robot: str):
        if not self._ok or self._task_pub is None:
            print("[ROS] dispatch skipped — ROS not ready yet")
            self.dispatchResult.emit(False, "ROS not connected — dispatch not sent")
            return

        import uuid

        req_id  = "eiu-" + uuid.uuid4().hex[:8]

        # Build patrol places: if loops > 1, prepend the robot's current waypoint
        # so the route is a real A->B trip instead of a no-op B->B.
        places = [place]
        if loops > 1:
            robots = json.loads(self._robots_json)
            home = self._nearest_wp_name(robots, robot)
            if home and home != place:
                places = [home, place]

        task_request = {
            "category": category,
            "description": {"places": places, "rounds": int(loops)},
            "unix_millis_earliest_start_time": 0,
            "requester": "eiu_fleet_ui",
        }
        # robot_task_request bypasses bidding entirely -- RMF assigns it to
        # this robot or rejects it outright, never to a different one.
        envelope = ({"type": "robot_task_request", "robot": robot,
                     "fleet": self._fleet_name, "request": task_request}
                    if robot else
                    {"type": "dispatch_task_request", "request": task_request})
        request_json = json.dumps(envelope)
        self._command_queue.put((req_id, request_json))
        print(f"[ROS] dispatch queued → place={place} loops={loops} "
              f"robot={robot or '(any)'} req_id={req_id}")

        rec = {
            "id":          req_id,
            "rmf_id":      "",           # filled in once dispatch_task_response arrives
            "date":        time.strftime("%d %b %Y"),
            "requester":   "eiu_fleet_ui",
            "pickup":      "n/a",
            "destination": place,
            "robot":       robot or "—",
            "start":       datetime.datetime.now().strftime("%I:%M:%S %p"),
            "end":         "—",
            "state":       "queued",
            "rounds":           int(loops),
            "rounds_remaining": int(loops),
            "_dispatched_at": time.monotonic(),   # for _check_dispatch_timeouts
        }
        with self._task_lock:
            self._tasks.insert(0, rec)
            del self._tasks[50:]          # trim in place; never rebind the list
            self._publish_tasks()

        self._planned_dest = place
        self.pathChanged.emit()
        self.dispatchResult.emit(True, "Dispatched")

    @Slot(str)
    def cancel_task(self, rmf_id: str):
        if not rmf_id:
            return
        if not self._ok or self._task_pub is None:
            self.dispatchResult.emit(False, "ROS not connected — cancel not sent")
            return

        import uuid
        req_id = "eiu-cancel-" + uuid.uuid4().hex[:8]
        request_json = json.dumps({
            "type":      "cancel_task_request",
            "task_id":   rmf_id,
            "requester": "eiu_fleet_ui",
            "labels":    [],
        })
        self._command_queue.put((req_id, request_json))
        print(f"[ROS] cancel_task queued → {rmf_id}")

        # Marked immediately since the dispatcher rarely confirms a cancel on
        # its own (same gap as task completion -- see _mark_task_completed).
        with self._task_lock:
            for task in self._tasks:
                if task.get("rmf_id") == rmf_id and task["state"] in ("queued", "underway"):
                    task["state"] = "cancelled"
                    if task.get("end", "—") == "—":
                        task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                    break
            self._publish_tasks()
        self.dispatchResult.emit(True, "Cancelled")

    # ── QML Properties ─────────────────────────────────────────────────────────

    @Property(bool, notify=rmfOnlineChanged)
    def rmfOnline(self):    return self._rmf_online

    @Property(str, notify=robotsChanged)
    def robotsJson(self):   return self._robots_json

    @Property(str, notify=tasksChanged)
    def tasksJson(self):    return self._tasks_json

    @Property(int, notify=trafficChanged)
    def blockedLanes(self):    return self._blocked_lanes

    @Property(str, notify=trafficChanged)
    def closedLaneIndicesJson(self): return self._closed_lane_indices_json

    @Property(int, notify=trafficChanged)
    def activeConflicts(self): return self._active_conflicts

    @Property(str, notify=pathChanged)
    def plannedDest(self):  return self._planned_dest
