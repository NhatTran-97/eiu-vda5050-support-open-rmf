"""
Cầu nối ROS 2 ↔ QML cho EIU Fleet UI.

  1. Subscribe  /fleet_states        (rmf_fleet_msgs/FleetState)  → bảng robot
  2. Subscribe  /task_api_responses  (rmf_task_msgs/ApiResponse)  → task state
  3. Publish    /task_api_requests   (rmf_task_msgs/ApiRequest)   → dispatch task
"""

import os
import json
import time
import datetime
import threading

from PySide6.QtCore import QObject, Signal, Property, Slot, QTimer

FLEET_STATES_TOPIC  = "/fleet_states"
TASK_API_TOPIC      = "/task_api_requests"
TASK_RESP_TOPIC     = "/task_api_responses"
OFFLINE_AFTER_SEC   = 5.0

_MODE_NAMES = {
    0: "IDLE", 1: "CHARGING", 2: "MOVING",  3: "PAUSED",
    4: "WAITING", 5: "EMERGENCY", 6: "GOING_HOME",
    7: "DOCKING", 8: "ERROR", 9: "CLEANING",
}

# RMF task status → label hiển thị
_STATE_LABEL = {
    "queued":      "queued",
    "selected":    "queued",
    "dispatching": "queued",
    "underway":    "underway",
    "completed":   "completed",
    "failed":      "failed",
    "cancelled":   "cancelled",
    "killed":      "failed",
    "blocked":     "underway",
    "skipped":     "cancelled",
}

_EPOCH_2020 = 1_577_836_800


def _fmt_time(ts) -> str:
    """Timestamp → 'HH:MM:SS AM/PM' (UTC). Trả '' nếu không hợp lệ."""
    if not ts:
        return ""
    ts = float(ts)
    if ts > 1e12:       # milliseconds → seconds
        ts /= 1000.0
    if ts <= 0:
        return ""
    return datetime.datetime.utcfromtimestamp(ts).strftime("%I:%M:%S %p")


def _finish_from_path(path) -> str:
    """Estimated finish time từ Location.t của waypoint cuối trong RMF path.
    RMF internal clock bắt đầu từ Unix epoch (0), nên t.sec=29375 → 08:09:35 UTC.
    Dùng UTC để hiển thị đúng giờ trong ngày.
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

    def __init__(self, parent=None):
        super().__init__(parent)

        self._robots_json  = "[]"
        self._rmf_online   = False
        self._last_rx      = 0.0
        self._tasks        = []       # list[dict], mới nhất ở đầu
        self._tasks_json   = "[]"
        self._planned_dest = ""
        self._waypoints    = []       # list[dict] {name,x,y} từ nav_graph
        self._task_miss    : dict[str, int] = {}  # rmf_id → consecutive miss count

        # request_id ("eiu-xxx") → rmf internal task_id
        self._req_to_rmf: dict[str, str] = {}

        self._node        = None
        self._task_pub    = None
        self._spin_thread = None
        self._ok          = False

        self._watchdog = QTimer(self)
        self._watchdog.setInterval(2000)
        self._watchdog.timeout.connect(self._check_online)

    def set_waypoints(self, wp_list: list):
        """Nhận danh sách waypoints từ MapProvider để dùng cho _nearest_wp_name."""
        self._waypoints = wp_list

    def _nearest_wp_name(self, robots: list) -> str:
        """Tìm waypoint gần nhất với vị trí robot đầu tiên."""
        if not robots or not self._waypoints:
            return ""
        r = robots[0]
        rx, ry = float(r.get("x", 0)), float(r.get("y", 0))
        best, best_d = "", float("inf")
        for wp in self._waypoints:
            d = (wp["x"] - rx) ** 2 + (wp["y"] - ry) ** 2
            if d < best_d:
                best_d = d
                best = wp["name"]
        return best

    # ── Khởi động / tắt ────────────────────────────────────────────────────────

    def start(self):
        domain = os.environ.get("EIU_ROS_DOMAIN_ID", "7")
        os.environ["ROS_DOMAIN_ID"] = domain

        try:
            import rclpy
            from rclpy.qos import (QoSProfile, QoSDurabilityPolicy,
                                   QoSReliabilityPolicy, QoSHistoryPolicy)
            from rmf_fleet_msgs.msg import FleetState
            from rmf_task_msgs.msg import ApiRequest, ApiResponse
        except Exception as e:
            print(f"[ROS] import lỗi ({e}). GUI chạy nhưng không có RMF data.")
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

            self._task_pub = self._node.create_publisher(
                ApiRequest, TASK_API_TOPIC, reliable_tl)

            self._node.create_subscription(
                FleetState, FLEET_STATES_TOPIC, self._on_fleet_state, 10)

            # Task state updates: dispatcher dùng TRANSIENT_LOCAL
            self._node.create_subscription(
                ApiResponse, TASK_RESP_TOPIC, self._on_task_response, reliable_tl)

            self._spin_thread = threading.Thread(target=self._spin, daemon=True)
            self._spin_thread.start()
            self._watchdog.start()
            self._ok = True
            print(f"[ROS] bridge online — domain {domain}")
        except Exception as e:
            print(f"[ROS] start lỗi: {e}")

    def _spin(self):
        import rclpy
        try:
            rclpy.spin(self._node)
        except Exception:
            pass

    @Slot()
    def shutdown(self):
        if not self._ok:
            return
        try:
            import rclpy
            self._watchdog.stop()
            if self._node:
                self._node.destroy_node()
            if rclpy.ok():
                rclpy.shutdown()
        except Exception:
            pass

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

        self._robots_json = json.dumps(list(current.values()))
        self._last_rx     = time.monotonic()

        if not self._rmf_online:
            self._rmf_online = True
            self.rmfOnlineChanged.emit()
        self.robotsChanged.emit()

        # Cross-reference: cập nhật bảng tasks theo robot đang làm gì
        self._sync_tasks_from_fleet(robot_task_map)

    def _robot_finish(self, robot_name: str, path) -> str:
        """Finish time: lấy từ path live khi đang chạy, fallback sang task gần nhất."""
        t = _finish_from_path(path)
        if t:
            return t
        for task in self._tasks:
            if task.get("robot") == robot_name and task.get("end", "—") != "—":
                return task["end"]
        return ""

    def _check_online(self):
        stale = (time.monotonic() - self._last_rx) > OFFLINE_AFTER_SEC
        if self._rmf_online and stale:
            self._rmf_online = False
            self.rmfOnlineChanged.emit()

    def _sync_tasks_from_fleet(self, robot_task_map: dict):
        """Cập nhật robot/state/end cho tasks dựa vào fleet_states."""
        updated = False
        active_rmf_ids = set(robot_task_map.keys())

        # rmf_ids đã được claim bởi task nào đó (dùng để fallback an toàn)
        claimed_ids = {t.get("rmf_id", "") for t in self._tasks}

        for task in self._tasks:
            rmf_id = task.get("rmf_id", "")

            if rmf_id:
                # Exact match bằng RMF task ID
                if rmf_id in active_rmf_ids:
                    self._task_miss.pop(rmf_id, None)   # reset miss counter
                    robot_name, finish = robot_task_map[rmf_id]
                    if task["robot"] != robot_name:
                        task["robot"] = robot_name; updated = True
                    if task["state"] == "queued":
                        task["state"] = "underway"; updated = True
                    if finish and task.get("end", "—") == "—":
                        task["end"] = finish; updated = True
                elif task["state"] == "underway":
                    # Grace period: chờ 3 lần liên tiếp không thấy mới mark completed
                    # tránh false-completed khi robot chuyển giữa 2 task
                    self._task_miss[rmf_id] = self._task_miss.get(rmf_id, 0) + 1
                    if self._task_miss[rmf_id] >= 3:
                        task["state"] = "completed"
                        if task.get("end", "—") == "—":
                            task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                        self._task_miss.pop(rmf_id, None)
                        updated = True

            elif task["state"] in ("queued", "underway") and robot_task_map:
                # Fallback: chưa nhận dispatch_task_response → chưa có rmf_id.
                # Chỉ gán rmf_id chưa được claim bởi task khác, tránh "đánh cắp"
                # rmf_id của task đang active và trigger false-completed.
                available = [(rid, info) for rid, info in robot_task_map.items()
                             if rid not in claimed_ids]
                if not available:
                    continue
                rmf_id_new, (robot_name, finish) = available[0]
                task["rmf_id"]  = rmf_id_new
                task["robot"]   = robot_name
                task["state"]   = "underway"
                if finish and task.get("end", "—") == "—":
                    task["end"] = finish
                claimed_ids.add(rmf_id_new)   # update local set ngay
                updated = True
                print(f"[ROS] fallback link: task {task['id']} → rmf_id={rmf_id_new} robot={robot_name}")

        if updated:
            self._tasks_json = json.dumps(self._tasks)
            self.tasksChanged.emit()

    # ── Subscribe: /task_api_responses ────────────────────────────────────────

    def _on_task_response(self, msg):
        """Nhận phản hồi từ Jazzy dispatcher: lấy rmf_task_id và state updates."""
        try:
            data = json.loads(msg.json_msg)
        except Exception:
            return

        msg_type = data.get("type", "")

        if msg_type == "dispatch_task_response":
            req_id  = msg.request_id          # "eiu-xxxxxxxx"
            rmf_id  = data.get("task_id", "")
            success = data.get("success", False)
            if not rmf_id:
                return
            self._req_to_rmf[req_id] = rmf_id
            print(f"[ROS] task mapped: {req_id} → {rmf_id} (success={success})")
            # Gắn rmf_id vào task record
            for task in self._tasks:
                if task["id"] == req_id:
                    task["rmf_id"] = rmf_id
                    if not success:
                        task["state"] = "failed"
                        self._tasks_json = json.dumps(self._tasks)
                        self.tasksChanged.emit()
                    break

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

            # Estimated finish time (có thể là giây hoặc milliseconds)
            finish_str = ""
            estimate = task_data.get("estimate", {})
            ft = estimate.get("finish_time") if estimate else None
            if ft:
                finish_str = _fmt_time(ft)

            updated = False
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
                self._tasks_json = json.dumps(self._tasks)
                self.tasksChanged.emit()

        else:
            # Log các type khác để debug
            if msg_type:
                print(f"[ROS] task_api_responses type={msg_type!r} (ignored)")

    # ── Publish: dispatch task ─────────────────────────────────────────────────

    @Slot(str, str, int)
    def dispatch(self, category: str, place: str, loops: int):
        if not self._ok or self._task_pub is None:
            print("[ROS] dispatch bỏ qua — ROS chưa sẵn sàng")
            return

        import uuid
        from rmf_task_msgs.msg import ApiRequest

        req_id  = "eiu-" + uuid.uuid4().hex[:8]

        # Xây dựng patrol places: nếu loops > 1, thêm waypoint hiện tại của robot
        # vào đầu để tạo route A→B thật sự thay vì B→B (no-op).
        places = [place]
        if loops > 1:
            robots = json.loads(self._robots_json)
            home = self._nearest_wp_name(robots)
            if home and home != place:
                places = [home, place]

        request = {
            "category": category,
            "description": {"places": places, "rounds": int(loops)},
            "unix_millis_earliest_start_time": 0,
            "requester": "eiu_fleet_ui",
        }
        msg = ApiRequest()
        msg.request_id = req_id
        msg.json_msg   = json.dumps({"type": "dispatch_task_request", "request": request})

        self._task_pub.publish(msg)
        print(f"[ROS] dispatch → place={place} loops={loops} req_id={req_id}")

        rec = {
            "id":          req_id,
            "rmf_id":      "",           # sẽ được điền khi nhận dispatch_task_response
            "date":        time.strftime("%d %b %Y"),
            "requester":   "eiu_fleet_ui",
            "pickup":      "n/a",
            "destination": place,
            "robot":       "—",
            "start":       datetime.datetime.now().strftime("%I:%M:%S %p"),
            "end":         "—",
            "state":       "queued",
        }
        self._tasks.insert(0, rec)
        self._tasks = self._tasks[:50]
        self._tasks_json = json.dumps(self._tasks)
        self.tasksChanged.emit()

        self._planned_dest = place
        self.pathChanged.emit()

    @Slot(str)
    def cancel_task(self, rmf_id: str):
        if not rmf_id:
            return
        if self._ok and self._task_pub is not None:
            import uuid
            from rmf_task_msgs.msg import ApiRequest
            msg = ApiRequest()
            msg.request_id = "eiu-cancel-" + uuid.uuid4().hex[:8]
            msg.json_msg   = json.dumps({
                "type":      "cancel_task_request",
                "task_id":   rmf_id,
                "requester": "eiu_fleet_ui",
                "labels":    [],
            })
            self._task_pub.publish(msg)
            print(f"[ROS] cancel_task → {rmf_id}")

        # Cập nhật local ngay, không chờ confirm từ dispatcher
        for task in self._tasks:
            if task.get("rmf_id") == rmf_id and task["state"] in ("queued", "underway"):
                task["state"] = "cancelled"
                if task.get("end", "—") == "—":
                    task["end"] = datetime.datetime.now().strftime("%I:%M:%S %p")
                break
        self._tasks_json = json.dumps(self._tasks)
        self.tasksChanged.emit()

    # ── QML Properties ─────────────────────────────────────────────────────────

    @Property(bool, notify=rmfOnlineChanged)
    def rmfOnline(self):    return self._rmf_online

    @Property(str, notify=robotsChanged)
    def robotsJson(self):   return self._robots_json

    @Property(str, notify=tasksChanged)
    def tasksJson(self):    return self._tasks_json

    @Property(str, notify=pathChanged)
    def plannedDest(self):  return self._planned_dest
