#!/usr/bin/env python3
"""
test_dispatch_e2e.py — end-to-end integration test for the vda5050_fleet_adapter.

Verifies the full dispatch -> navigate -> complete loop using a simulated AGV
(mock_mqtt_robot.py), with no hardware. It:
  1. launches the mock robot (unless --no-mock),
  2. dispatches a patrol to a target waypoint via dispatch_patrol.py,
  3. watches MQTT and asserts the adapter published at least one `order` AND the
     robot reported arrival (lastNodeId == target),
  4. exits 0 on PASS, 1 on FAIL.

Prerequisites (already running, same ROS_DOMAIN_ID):
  ros2 run rmf_traffic_ros2 rmf_traffic_schedule
  ros2 run rmf_task_ros2 rmf_task_dispatcher
  ros2 launch vda5050_fleet_adapter fleet_adapter.launch.py

Usage:
  python3 test_dispatch_e2e.py --target wp6
  python3 test_dispatch_e2e.py --target wp2_parking --no-mock   # external robot
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
    """Tracks orders published by the adapter and the robot's reported node."""

    def __init__(self, base: str, target: str):
        self.base = base
        self.target = target
        self.orders = 0
        self.reached = threading.Event()
        self.last_node = None
        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, rc):
        client.subscribe(f"{self.base}/order", qos=1)
        client.subscribe(f"{self.base}/state", qos=1)

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
        except Exception:
            return
        if msg.topic.endswith("/order"):
            self.orders += 1
        elif msg.topic.endswith("/state"):
            self.last_node = payload.get("lastNodeId")
            if self.last_node == self.target and not payload.get("driving", False):
                self.reached.set()

    def start(self, host, port):
        self.client.connect(host, port, keepalive=30)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()


def main() -> int:
    p = argparse.ArgumentParser(description="E2E test for vda5050_fleet_adapter.")
    p.add_argument("--target", default="wp6", help="waypoint the robot must reach")
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--interface", default="TB3")
    p.add_argument("--manufacturer", default="ROBOTIS")
    p.add_argument("--serial", default="0001")
    p.add_argument("--timeout", type=float, default=90.0,
                   help="seconds to wait for arrival")
    p.add_argument("--settle", type=float, default=6.0,
                   help="seconds to let the adapter add the robot before dispatch")
    p.add_argument("--no-mock", action="store_true",
                   help="do not launch the mock robot (use a real/external one)")
    args = p.parse_args()

    base = f"{args.interface}/v2/{args.manufacturer}/{args.serial}"
    watcher = Watcher(base, args.target)
    watcher.start(args.host, args.port)

    mock = None
    if not args.no_mock:
        print("[test] launching mock robot ...")
        mock = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "mock_mqtt_robot.py"),
             "--host", args.host, "--port", str(args.port),
             "--interface", args.interface, "--manufacturer", args.manufacturer,
             "--serial", args.serial])

    try:
        print(f"[test] waiting {args.settle:.0f}s for the adapter to add the robot ...")
        time.sleep(args.settle)

        print(f"[test] dispatching patrol -> {args.target}")
        dispatch = subprocess.run(
            [sys.executable, os.path.join(HERE, "dispatch_patrol.py"), args.target],
            capture_output=True, text=True)
        sys.stdout.write(dispatch.stdout)
        if dispatch.returncode != 0:
            sys.stderr.write(dispatch.stderr)
            print("[test] FAIL: dispatch command failed")
            return 1

        print(f"[test] waiting up to {args.timeout:.0f}s for arrival at "
              f"'{args.target}' ...")
        ok = watcher.reached.wait(timeout=args.timeout)

        print(f"[test] orders published by adapter: {watcher.orders}")
        print(f"[test] last reported node: {watcher.last_node}")

        if ok and watcher.orders > 0:
            print(f"[test] PASS: robot reached '{args.target}' "
                  f"after {watcher.orders} order(s)")
            return 0
        if watcher.orders == 0:
            print("[test] FAIL: adapter never published an order "
                  "(robot not added? dispatch not awarded?)")
        else:
            print(f"[test] FAIL: timed out before reaching '{args.target}' "
                  f"(stuck at '{watcher.last_node}')")
        return 1
    finally:
        watcher.stop()
        if mock is not None:
            mock.terminate()
            try:
                mock.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mock.kill()


if __name__ == "__main__":
    sys.exit(main())
