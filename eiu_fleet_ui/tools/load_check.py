#!/usr/bin/env python3
"""Drive the dashboard offscreen with synthetic fleets and report how much of the UI it rebuilds.

Robots walk the nav graph; /fleet_states and VDA5050 state messages go through the same backend
callbacks the ROS and MQTT threads call, from threads of their own. Nothing connects to ROS or a broker.
"""
import argparse
import json
import math
import os
import random
import sys
import tempfile
import threading
import time
from pathlib import Path
from types import SimpleNamespace

PKG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PKG_ROOT))


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fleets", type=int, default=2, help="number of fleets")
    parser.add_argument("--robots", type=int, default=6, help="robots per fleet")
    parser.add_argument("--fleet-hz", type=float, default=10.0, help="/fleet_states rate per fleet")
    parser.add_argument("--state-hz", type=float, default=5.0, help="VDA5050 state rate per robot")
    parser.add_argument("--seconds", type=float, default=10.0, help="measured run time")
    parser.add_argument("--tasks", type=int, default=8, help="tasks put in the task table")
    parser.add_argument("--offline", type=int, default=1, help="robots that report OFFLINE")
    parser.add_argument("--size", default="1920x1080", help="window size")
    parser.add_argument("--out", default=str(Path(tempfile.gettempdir()) / "eiu_load_check"), help="screenshot folder")
    parser.add_argument("--report", help="also write the report as JSON to this file")
    return parser.parse_args()


ARGS = parse_args()
WORK = Path(tempfile.mkdtemp(prefix="eiu_load_check_"))
os.environ["QT_QPA_PLATFORM"] = "offscreen"
os.environ.setdefault("QT_QUICK_BACKEND", "software")
os.environ.pop("QT_SCALE_FACTOR", None)
os.environ["EIU_CONFIG_DIR"] = str(WORK / "state")
os.environ["XDG_CONFIG_HOME"] = str(WORK / "xdg")
os.environ.setdefault("EIU_NAV_GRAPH", str(PKG_ROOT / "maps" / "nav_graph.yaml"))


def write_fleet_configs() -> list[tuple[str, list[str]]]:
    """One adapter config per fleet in the work folder; returns (fleet, robot names)."""
    fleets, entries = [], []
    for f in range(ARGS.fleets):
        fleet = f"load_fleet_{f + 1}"
        robots = {f"f{f + 1}_r{i + 1}": {"manufacturer": "LOADTEST", "serial": f"{f + 1:02d}{i + 1:03d}"}
                  for i in range(ARGS.robots)}
        config = {"rmf_fleet": {"name": fleet, "task_capabilities": {"loop": True, "delivery": True}},
                  "vda5050": {"interface_name": "LOAD", "mqtt": {"host": "127.0.0.1", "port": 1}, "robots": robots}}
        path = WORK / "config" / f"config_{fleet}.yaml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(config))
        entries.append(f"{path}=adapter_{fleet}")
        fleets.append((fleet, list(robots)))
    os.environ["EIU_FLEET_ADAPTERS"] = ",".join(entries)
    return fleets


FLEETS = write_fleet_configs()

from PySide6.QtCore import QTimer, qInstallMessageHandler, QtMsgType  # noqa: E402
from PySide6.QtQuick import QQuickItem  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402


class Walker:
    """A robot moving along random lanes of the nav graph."""

    def __init__(self, name, waypoints, lanes, speed=0.5):
        self.name = name
        self.points = [(w["x"], w["y"]) for w in waypoints]
        self.names = [w.get("name", "") for w in waypoints]
        self.next_of = {}
        for lane in lanes:
            self.next_of.setdefault(lane["from"], []).append(lane["to"])
            if lane.get("bidir"):
                self.next_of.setdefault(lane["to"], []).append(lane["from"])
        self.at = random.choice(list(self.next_of) or [0])
        self.to = self._pick(self.at)
        self.t = random.random()
        self.speed = speed
        self.seq = 0

    def _pick(self, index):
        return random.choice(self.next_of.get(index) or [index])

    def step(self, dt):
        ax, ay = self.points[self.at]
        bx, by = self.points[self.to]
        length = max(0.01, math.hypot(bx - ax, by - ay))
        self.t += self.speed * dt / length
        while self.t >= 1.0:
            self.t -= 1.0
            self.at, self.to = self.to, self._pick(self.to)
            self.seq += 2
            ax, ay = self.points[self.at]
            bx, by = self.points[self.to]
        return ax + (bx - ax) * self.t, ay + (by - ay) * self.t, math.atan2(by - ay, bx - ax)


def fleet_message(fleet, walkers, task_of):
    robots = []
    for w in walkers:
        x, y, yaw = w.pose
        bx, by = w.points[w.to]
        robots.append(SimpleNamespace(
            name=w.name, model="load", task_id=task_of.get(w.name, ""),
            mode=SimpleNamespace(mode=2), battery_percent=80.0 - w.seq * 0.1,
            location=SimpleNamespace(x=x, y=y, yaw=yaw, level_name="L1"),
            path=[SimpleNamespace(x=bx, y=by, t=SimpleNamespace(sec=int(time.time()) + 30))]))
    return SimpleNamespace(name=fleet, robots=robots)


def state_payload(w):
    x, y, yaw = w.pose
    node = w.names[w.to] or f"n{w.to}"
    return json.dumps({
        "orderId": f"order-{w.name}", "orderUpdateId": w.seq, "lastNodeId": w.names[w.at] or f"n{w.at}",
        "lastNodeSequenceId": w.seq, "driving": True, "paused": False, "operatingMode": "AUTOMATIC",
        "distanceSinceLastNode": round(w.t, 2), "velocity": {"vx": 0.5, "vy": 0.0},
        "agvPosition": {"x": x, "y": y, "theta": yaw, "mapId": "L1", "positionInitialized": True},
        "batteryState": {"batteryCharge": 80.0 - w.seq * 0.1, "charging": False},
        "safetyState": {"eStop": "NONE", "fieldViolation": False},
        "nodeStates": [{"nodeId": node, "sequenceId": w.seq + 2, "released": True}],
        "edgeStates": [], "actionStates": [], "errors": [], "loads": []}).encode()


class Churn:
    """Counts Qt Quick items that appear in the window: a rebuilt delegate is a new item."""

    def __init__(self, window):
        self.window = window
        self.created = 0
        self.samples = 0

    def sample(self):
        stack = [self.window.contentItem()]
        new = 0
        while stack:
            item = stack.pop()
            if item.property("_loadCheckSeen") is None:
                item.setProperty("_loadCheckSeen", True)
                new += 1
            stack.extend(item.childItems())
        self.samples += 1
        return new


def find_robot_list(window):
    """The ListView of the robot panel."""
    panel = window.findChild(QQuickItem, "activeRobotsPanel")
    stack = [panel] if panel else []
    while stack:
        item = stack.pop()
        if item.inherits("QQuickListView"):
            return item
        stack.extend(item.childItems())
    return None


def main():
    warnings = []

    def on_message(kind, _ctx, text):
        if kind in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg):
            warnings.append(text)

    qInstallMessageHandler(on_message)
    from eiu_fleet_ui.main import build_engine

    app = QApplication(sys.argv[:1])
    app.setOrganizationName("EIU-load-check")
    app.setApplicationName("EIU Fleet UI load check")
    engine, backends = build_engine(app)
    if not engine.rootObjects():
        sys.exit("main.qml failed to load")
    window = engine.rootObjects()[0]
    width, height = (int(v) for v in ARGS.size.split("x"))
    window.setProperty("visibility", 2)
    window.resize(width, height)
    window.show()

    ros, mqtt, map_prov = backends.ros, backends.mqtt, backends.map_prov
    ros.set_waypoints(map_prov.waypoints())
    waypoints = map_prov.waypoints()
    lanes = json.loads(map_prov.lanesJson)
    identities = {r.name: r for r in backends.settings._robots}

    walkers = {fleet: [Walker(name, waypoints, lanes) for name in names] for fleet, names in FLEETS}
    for fleet_walkers in walkers.values():
        for w in fleet_walkers:
            w.pose = w.step(0.0)

    # Tasks: some underway on robots, some queued.
    ros._ok, ros._task_pub = True, object()
    task_of = {}
    names = [w.name for ws in walkers.values() for w in ws]
    places = [w["name"] for w in waypoints if w.get("name")]
    for i in range(ARGS.tasks):
        request_id = ros.dispatch("patrol", random.choice(places), 1)
        if i < len(names) and i % 2 == 0:
            rmf_id = f"patrol.load-{i}"
            ros._attach_rmf_id(request_id, rmf_id, True)
            task_of[names[i]] = rmf_id

    offline = set(names[:ARGS.offline])
    lock = threading.Lock()
    stop = threading.Event()

    def move(dt):
        with lock:
            for fleet_walkers in walkers.values():
                for w in fleet_walkers:
                    w.pose = w.step(dt)

    def rmf_thread():
        period = 1.0 / ARGS.fleet_hz
        last = time.monotonic()
        while not stop.is_set():
            now = time.monotonic()
            move(now - last)
            last = now
            for fleet, fleet_walkers in walkers.items():
                with lock:
                    msg = fleet_message(fleet, fleet_walkers, task_of)
                ros._on_fleet_state(msg)
            time.sleep(period)

    def mqtt_thread():
        period = 1.0 / ARGS.state_hz
        for name, identity in identities.items():
            state = "OFFLINE" if name in offline else "ONLINE"
            mqtt._on_message(None, None, SimpleNamespace(topic=identity.topic("connection"),
                                                         payload=json.dumps({"connectionState": state}).encode()))
        while not stop.is_set():
            for fleet_walkers in walkers.values():
                for w in fleet_walkers:
                    if w.name in offline:
                        continue
                    with lock:
                        payload = state_payload(w)
                    mqtt._on_message(None, None, SimpleNamespace(topic=identities[w.name].topic("state"), payload=payload))
            time.sleep(period)

    churn = Churn(window)
    report = {}
    threads = [threading.Thread(target=rmf_thread, daemon=True), threading.Thread(target=mqtt_thread, daemon=True)]
    for t in threads:
        t.start()

    warmup_ms = 3000
    state = {"created": 0, "cpu0": 0.0, "wall0": 0.0, "scroll_set": None}

    def begin():
        churn.sample()
        state["cpu0"], state["wall0"] = time.process_time(), time.monotonic()
        robot_list = find_robot_list(window)
        if robot_list is not None:
            target = max(0.0, robot_list.property("contentHeight") - robot_list.property("height")) / 2
            robot_list.setProperty("contentY", target)
            state["scroll_set"] = robot_list.property("contentY")
        sampler.start()

    def tick():
        state["created"] += churn.sample()

    def finish():
        sampler.stop()
        tick()
        cpu = time.process_time() - state["cpu0"]
        wall = time.monotonic() - state["wall0"]
        stop.set()
        robot_list = find_robot_list(window)
        out = Path(ARGS.out)
        out.mkdir(parents=True, exist_ok=True)
        shot = out / f"load_{ARGS.fleets}x{ARGS.robots}.png"
        window.grabWindow().save(str(shot))
        report.update({
            "robots": len(names), "fleet_hz": ARGS.fleet_hz, "state_hz": ARGS.state_hz, "seconds": round(wall, 1),
            "items_created_per_s": round(state["created"] / wall, 1),
            "cpu_cores": round(cpu / wall, 2),
            "robot_list_scroll": (None if state["scroll_set"] is None or robot_list is None else
                                  {"set": round(state["scroll_set"], 1), "after": round(robot_list.property("contentY"), 1)}),
            "qml_warnings": len(warnings),
            "screenshot": str(shot)})
        app.quit()

    sampler = QTimer()
    sampler.setInterval(250)
    sampler.timeout.connect(tick)
    QTimer.singleShot(warmup_ms, begin)
    QTimer.singleShot(warmup_ms + int(ARGS.seconds * 1000), finish)
    app.exec()

    print(json.dumps(report, indent=2))
    if ARGS.report:
        Path(ARGS.report).write_text(json.dumps(report))
    for text in sorted(set(warnings))[:10]:
        print("  warning:", text[:200])


if __name__ == "__main__":
    main()
