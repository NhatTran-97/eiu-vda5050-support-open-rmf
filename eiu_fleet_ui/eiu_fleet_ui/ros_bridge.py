"""
Cầu nối ROS 2 ↔ QML cho EIU Fleet UI.

Hai nhiệm vụ:
  1. Subscribe  /fleet_states      (rmf_fleet_msgs/FleetState)  → bảng robot
  2. Publish    /task_api_requests (rmf_task_msgs/ApiRequest)   → dispatch task

QML dùng:
    ros.rmfOnline     → bool   (có nhận được fleet_states gần đây không)
    ros.robotsJson    → str    (JSON list robot: name, fleet, status, level,
                                 battery, task, x, y)
    ros.dispatch(category, place, loops)   → gửi task patrol cho RMF

Kiến trúc luồng:
    rclpy.spin chạy trong 1 thread nền (daemon). Callback fleet_states cập nhật
    dữ liệu rồi emit signal Qt — signal được Qt queue về main thread, QML đọc
    property trên main thread nên an toàn.

RMF core chạy ở ROS_DOMAIN_ID = 7 (host mặc định 4) nên bridge ÉP domain 7
trước khi rclpy.init — chỉnh bằng env EIU_ROS_DOMAIN_ID nếu cần.
"""

import os
import json
import time
import threading

from PySide6.QtCore import QObject, Signal, Property, Slot, QTimer

# Topic + hằng số
FLEET_STATES_TOPIC = "/fleet_states"
TASK_API_TOPIC     = "/task_api_requests"
OFFLINE_AFTER_SEC  = 5.0        # không có fleet_states quá ngần này → coi RMF offline

# rmf_fleet_msgs/RobotMode.mode  →  chuỗi hiển thị
_MODE_NAMES = {
    0: "IDLE",
    1: "CHARGING",
    2: "MOVING",
    3: "PAUSED",
    4: "WAITING",
    5: "EMERGENCY",
    6: "GOING_HOME",
    7: "DOCKING",
    8: "ERROR",
    9: "CLEANING",
}


class RosBridge(QObject):

    robotsChanged    = Signal()
    rmfOnlineChanged = Signal()
    tasksChanged     = Signal()
    pathChanged      = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)

        self._robots_json = "[]"     # QML đọc trực tiếp chuỗi này
        self._rmf_online  = False
        self._last_rx     = 0.0      # thời điểm nhận fleet_states cuối (monotonic)
        self._tasks       = []       # task đã dispatch từ UI (mới nhất ở đầu)
        self._tasks_json  = "[]"
        self._planned_dest = ""      # tên waypoint đích đang được dispatch

        # ROS objects — tạo trong start()
        self._node        = None
        self._task_pub    = None
        self._spin_thread = None
        self._ok          = False    # ROS init thành công chưa

        # Timer kiểm tra RMF online/offline (chạy trên main thread)
        self._watchdog = QTimer(self)
        self._watchdog.setInterval(2000)
        self._watchdog.timeout.connect(self._check_online)

    # ── Khởi động / tắt ────────────────────────────────────────────────────────

    def start(self):
        """Gọi từ main.py. Import ROS lazily để GUI vẫn chạy nếu chưa source ROS."""
        domain = os.environ.get("EIU_ROS_DOMAIN_ID", "7")
        os.environ["ROS_DOMAIN_ID"] = domain   # ÉP domain trước khi init

        try:
            import rclpy
            from rclpy.qos import (QoSProfile, QoSDurabilityPolicy,
                                   QoSReliabilityPolicy, QoSHistoryPolicy)
            from rmf_fleet_msgs.msg import FleetState
            from rmf_task_msgs.msg import ApiRequest
        except Exception as e:
            print(f"[ROS] không import được ROS/rmf msgs ({e}). "
                  f"GUI vẫn chạy nhưng không có dữ liệu robot/dispatch.")
            return

        try:
            if not rclpy.ok():
                rclpy.init(args=None)
            self._node = rclpy.create_node("eiu_fleet_ui_bridge")

            # QoS cho dispatch: RELIABLE + TRANSIENT_LOCAL (dispatcher yêu cầu)
            task_qos = QoSProfile(
                history=QoSHistoryPolicy.KEEP_LAST,
                depth=10,
                reliability=QoSReliabilityPolicy.RELIABLE,
                durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
            )
            self._task_pub = self._node.create_publisher(
                ApiRequest, TASK_API_TOPIC, task_qos)

            # Subscribe fleet_states (QoS mặc định: reliable/volatile/keep_last 10)
            self._node.create_subscription(
                FleetState, FLEET_STATES_TOPIC, self._on_fleet_state, 10)

            self._spin_thread = threading.Thread(target=self._spin, daemon=True)
            self._spin_thread.start()
            self._watchdog.start()
            self._ok = True
            print(f"[ROS] bridge online — domain {domain}, "
                  f"subscribe {FLEET_STATES_TOPIC}, publish {TASK_API_TOPIC}")
        except Exception as e:
            print(f"[ROS] start lỗi: {e}")

    def _spin(self):
        import rclpy
        try:
            rclpy.spin(self._node)
        except Exception:
            pass   # shutdown làm spin thoát — bỏ qua

    @Slot()
    def shutdown(self):
        if not self._ok:
            return
        try:
            import rclpy
            self._watchdog.stop()
            if self._node is not None:
                self._node.destroy_node()
            if rclpy.ok():
                rclpy.shutdown()
        except Exception:
            pass

    # ── Subscribe: /fleet_states ───────────────────────────────────────────────

    def _on_fleet_state(self, msg):
        """Chạy trong spin thread. Dựng lại list robot của fleet này."""
        fleet = msg.name

        # Copy dict cũ, bỏ robot thuộc fleet này, thêm lại từ message mới
        current = {r["key"]: r for r in json.loads(self._robots_json)}
        current = {k: v for k, v in current.items() if v["fleet"] != fleet}

        for r in msg.robots:
            loc = r.location
            key = f"{fleet}/{r.name}"
            # mode.mode 0 = IDLE thường không được set đúng bởi EasyFullControl;
            # nếu robot đang có task thì hiển thị WORKING thay vì IDLE
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
                "finish":  "",                          # Est. finish — cần task data (TODO)
                "updated": time.strftime("%H:%M:%S"),   # Last Updated (giờ nhận state)
                "x":       float(loc.x),
                "y":       float(loc.y),
                "yaw":     float(loc.yaw),
                "path":    [{"x": float(p.x), "y": float(p.y)} for p in r.path],
            }

        # Swap nguyên tham chiếu (an toàn với getter ở main thread)
        self._robots_json = json.dumps(list(current.values()))
        self._last_rx     = time.monotonic()

        if not self._rmf_online:
            self._rmf_online = True
            self.rmfOnlineChanged.emit()
        self.robotsChanged.emit()

    def _check_online(self):
        """Main thread (QTimer): hết hạn fleet_states → RMF offline."""
        stale = (time.monotonic() - self._last_rx) > OFFLINE_AFTER_SEC
        if self._rmf_online and stale:
            self._rmf_online = False
            self.rmfOnlineChanged.emit()

    # ── Publish: dispatch task ─────────────────────────────────────────────────

    @Slot(str, str, int)
    def dispatch(self, category: str, place: str, loops: int):
        """Gửi 1 task patrol tới RMF. Gọi từ QML (New Task dialog → Submit)."""
        if not self._ok or self._task_pub is None:
            print("[ROS] dispatch bỏ qua — ROS chưa sẵn sàng")
            return

        import uuid
        from rmf_task_msgs.msg import ApiRequest

        request = {
            "category": category,
            "description": {"places": [place], "rounds": int(loops)},
            "unix_millis_earliest_start_time": 0,
            "requester": "eiu_fleet_ui",
        }
        msg = ApiRequest()
        msg.request_id = "eiu-" + uuid.uuid4().hex[:8]
        msg.json_msg   = json.dumps(
            {"type": "dispatch_task_request", "request": request})

        subs = self._task_pub.get_subscription_count()
        self._task_pub.publish(msg)
        print(f"[ROS] dispatch patrol → place={place} rounds={loops} "
              f"(subs={subs}, request_id={msg.request_id})")

        # Ghi lại task vào bảng Tasks (cập nhật state thật cần task_states — TODO)
        rec = {
            "id":          msg.request_id,
            "date":        time.strftime("%d %b %Y"),
            "requester":   "eiu_fleet_ui",
            "pickup":      "n/a",
            "destination": place,
            "robot":       "—",
            "start":       time.strftime("%I:%M:%S %p"),
            "end":         "—",
            "state":       "queued",
        }
        self._tasks.insert(0, rec)
        self._tasks = self._tasks[:50]
        self._tasks_json = json.dumps(self._tasks)
        self.tasksChanged.emit()

        self._planned_dest = place
        self.pathChanged.emit()

    # ── QML Properties ─────────────────────────────────────────────────────────

    @Property(bool, notify=rmfOnlineChanged)
    def rmfOnline(self):
        return self._rmf_online

    @Property(str, notify=robotsChanged)
    def robotsJson(self):
        return self._robots_json

    @Property(str, notify=tasksChanged)
    def tasksJson(self):
        return self._tasks_json

    @Property(str, notify=pathChanged)
    def plannedDest(self):
        return self._planned_dest
