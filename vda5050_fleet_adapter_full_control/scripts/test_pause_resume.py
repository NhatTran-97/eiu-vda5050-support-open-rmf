#!/usr/bin/env python3
"""
test_pause_resume.py — checks that an operator pause holds the robot without
throwing its order away.

RMF stops a robot as part of interrupting it, and a naive adapter forwards
that stop as a VDA5050 cancelOrder — which discards the order the pause is
supposed to preserve. This test dispatches a long route, pauses mid-way, and
fails if a cancelOrder shows up.

It watches MQTT directly, so it sees what the AGV sees:
  PASS  startPause arrives, no cancelOrder, stopPause on resume, and the
        robot keeps working the same order afterwards.
  FAIL  a cancelOrder appears, or a second order is issued on resume.

Prerequisites (already running, same ROS_DOMAIN_ID):
  ros2 run rmf_traffic_ros2 rmf_traffic_schedule
  ros2 run rmf_task_ros2 rmf_task_dispatcher
  ros2 launch vda5050_fleet_adapter_full_control fleet_adapter.launch.py

Usage:
  python3 test_pause_resume.py --target Patrol_F3
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

import paho.mqtt.client as mqtt

HERE = os.path.dirname(os.path.abspath(__file__))


class Watcher:
    """Records the orders and instant actions the adapter publishes."""

    def __init__(self, base: str):
        self.base = base
        self.lock = threading.Lock()
        self.orders = []          # order ids, in the order they were sent
        self.actions = []         # (timestamp, actionType)
        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, rc):
        client.subscribe(f"{self.base}/order", qos=1)
        client.subscribe(f"{self.base}/instantActions", qos=1)

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
        except Exception:
            return
        with self.lock:
            if msg.topic.endswith("/order"):
                self.orders.append(payload.get("orderId", "")[:8])
            elif msg.topic.endswith("/instantActions"):
                for a in payload.get("actions", []):
                    self.actions.append((time.time(), a.get("actionType", "")))

    def since(self, when: float):
        with self.lock:
            return [t for ts, t in self.actions if ts >= when]

    def order_count(self):
        with self.lock:
            return len(self.orders)

    def start(self, host, port):
        self.client.connect(host, port, keepalive=30)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()


def call_service(node: str, robot: str, verb: str) -> str:
    result = subprocess.run(
        ["ros2", "service", "call", f"/{node}/{robot}/{verb}",
         "std_srvs/srv/Trigger"],
        capture_output=True, text=True, timeout=30)
    return result.stdout.strip().splitlines()[-1] if result.stdout else result.stderr


def main() -> int:
    p = argparse.ArgumentParser(description="Pause/resume test for the VDA5050 adapter.")
    p.add_argument("--target", default="Patrol_F3", help="a waypoint far from the robot")
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--interface", default="TB3")
    p.add_argument("--manufacturer", default="ROBOTIS")
    p.add_argument("--serial", default="0001")
    p.add_argument("--node", default="vda5050_fleet_adapter_full_control",
                   help="adapter node name the services live under")
    p.add_argument("--robot", default="tb3_1")
    p.add_argument("--step-time", type=float, default=1.0,
                   help="mock robot seconds per 0.3 m step -- slow enough to pause")
    p.add_argument("--drive-before-pause", type=float, default=12.0,
                   help="seconds of driving before the pause is issued")
    p.add_argument("--hold", type=float, default=8.0,
                   help="seconds to stay paused while watching for a cancelOrder")
    args = p.parse_args()

    # A stale mock on the same identity makes every observation meaningless.
    subprocess.run(["pkill", "-f", "mock_mqtt_robot.py"], capture_output=True)
    time.sleep(1.0)

    base = f"{args.interface}/v2/{args.manufacturer}/{args.serial}"
    watcher = Watcher(base)
    watcher.start(args.host, args.port)

    print("[test] launching mock robot ...")
    mock = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock_mqtt_robot.py"),
         "--host", args.host, "--port", str(args.port),
         "--interface", args.interface, "--manufacturer", args.manufacturer,
         "--serial", args.serial, "--step-time", str(args.step_time)])

    try:
        print("[test] waiting 8s for the adapter to add the robot ...")
        time.sleep(8.0)

        print(f"[test] dispatching patrol -> {args.target}")
        dispatch = subprocess.run(
            [sys.executable, os.path.join(HERE, "dispatch_patrol.py"), args.target],
            capture_output=True, text=True)
        sys.stdout.write(dispatch.stdout)
        if dispatch.returncode != 0:
            sys.stderr.write(dispatch.stderr)
            print("[test] FAIL: dispatch command failed")
            return 1

        print(f"[test] letting it drive for {args.drive_before_pause:.0f}s ...")
        time.sleep(args.drive_before_pause)

        orders_before = watcher.order_count()
        if orders_before == 0:
            print("[test] FAIL: the adapter never published an order")
            return 1

        mark = time.time()
        print(f"[test] pausing '{args.robot}' ...")
        print("       " + call_service(args.node, args.robot, "pause"))
        time.sleep(args.hold)

        during_pause = watcher.since(mark)
        print(f"[test] instant actions during the pause: {during_pause or '(none)'}")

        failures = []
        if "cancelOrder" in during_pause:
            failures.append("cancelOrder was sent -- the pause threw the order away")
        if "startPause" not in during_pause:
            failures.append("no startPause reached the AGV")

        mark = time.time()
        print(f"[test] resuming '{args.robot}' ...")
        print("       " + call_service(args.node, args.robot, "resume"))
        time.sleep(3.0)

        during_resume = watcher.since(mark)
        print(f"[test] instant actions during the resume: {during_resume or '(none)'}")
        if "stopPause" not in during_resume:
            failures.append("no stopPause reached the AGV")

        orders_after = watcher.order_count()
        if orders_after > orders_before:
            failures.append(
                f"{orders_after - orders_before} new order(s) after the resume -- the "
                "route was rebuilt instead of continued")

        if failures:
            for f in failures:
                print(f"[test] FAIL: {f}")
            return 1

        print(f"[test] PASS: paused and resumed on the same order "
              f"({orders_after} order(s) total)")
        return 0
    finally:
        watcher.stop()
        mock.terminate()
        try:
            mock.wait(timeout=5)
        except subprocess.TimeoutExpired:
            mock.kill()


if __name__ == "__main__":
    sys.exit(main())
