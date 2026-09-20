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

With --config (repeatable) it tests several fleets at once: one mock robot per
fleet config (its first robot), and one patrol per fleet given by
--fleet-target FLEET=WAYPOINT. Fleet adapters must be started with the same
configs; the mock robots reuse their identities, so the broker must be local.

Usage:
  python3 test_dispatch_e2e.py --target wp6
  python3 test_dispatch_e2e.py --target wp2_parking --no-mock   # external robot
  python3 test_dispatch_e2e.py --host localhost --port 1883 \
      --config config_amr.yaml --config config_tb3.yaml \
      --fleet-target amr_fleet=Patrol_C3 --fleet-target tb3_fleet=Patrol_C1
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
sys.path.insert(0, HERE)

import run_mock_fleets  # noqa: E402


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


def run_fleets(args) -> int:
    """Dispatch one patrol per fleet config and wait for every robot to arrive."""
    graph = run_mock_fleets.find_nav_graph(args.nav_graph)
    targets = dict(item.split("=", 1) for item in args.fleet_target)

    robots = []
    for config in args.config:
        robots.append(run_mock_fleets.load_robots(config, graph)[0])
    for robot in robots:
        robot["target"] = targets.get(robot["fleet"], args.target)
        if args.host:
            robot["host"] = args.host
        if args.port:
            robot["port"] = args.port
        if not args.allow_remote:
            run_mock_fleets.check_local_broker(robot["host"])

    watchers = []
    for robot in robots:
        base = f"{robot['interface']}/v2/{robot['manufacturer']}/{robot['serial']}"
        watcher = Watcher(base, robot["target"])
        watcher.start(robot["host"], robot["port"])
        watchers.append(watcher)

    mocks = []
    if not args.no_mock:
        for robot in robots:
            print(f"[test] launching mock {robot['fleet']}/{robot['name']} ...")
            mocks.append(run_mock_fleets.spawn_mock(robot))

    try:
        print(f"[test] waiting {args.settle:.0f}s for the adapters to add the robots ...")
        time.sleep(args.settle)

        for robot in robots:
            print(f"[test] dispatching {robot['fleet']} -> {robot['target']}")
            dispatch = subprocess.run(
                [sys.executable, os.path.join(HERE, "dispatch_patrol.py"),
                 "--fleet", robot["fleet"], robot["target"]],
                capture_output=True, text=True)
            if dispatch.returncode != 0:
                sys.stderr.write(dispatch.stderr)
                print(f"[test] FAIL: dispatch to {robot['fleet']} failed")
                return 1

        deadline = time.time() + args.timeout
        results = []
        for robot, watcher in zip(robots, watchers):
            ok = watcher.reached.wait(timeout=max(0.0, deadline - time.time()))
            results.append(ok and watcher.orders > 0)
            print(f"[test] {robot['fleet']}/{robot['name']}: orders={watcher.orders} "
                  f"last node={watcher.last_node} target={robot['target']} "
                  f"-> {'reached' if results[-1] else 'NOT reached'}")

        if all(results):
            print(f"[test] PASS: all {len(robots)} fleets reached their targets")
            return 0
        print("[test] FAIL: not every fleet reached its target")
        return 1
    finally:
        for watcher in watchers:
            watcher.stop()
        for mock in mocks:
            mock.terminate()


def main() -> int:
    p = argparse.ArgumentParser(description="E2E test for vda5050_fleet_adapter.")
    p.add_argument("--target", default="wp6", help="waypoint the robot must reach")
    p.add_argument("--host", default=None,
                   help="broker host (default localhost; with --config, the configs' broker)")
    p.add_argument("--port", type=int, default=None,
                   help="broker port (default 1883; with --config, the configs' broker)")
    p.add_argument("--interface", default="TB3")
    p.add_argument("--manufacturer", default="ROBOTIS")
    p.add_argument("--serial", default="0001")
    p.add_argument("--timeout", type=float, default=90.0,
                   help="seconds to wait for arrival")
    p.add_argument("--settle", type=float, default=6.0,
                   help="seconds to let the adapter add the robot before dispatch")
    p.add_argument("--no-mock", action="store_true",
                   help="do not launch the mock robot (use a real/external one)")
    p.add_argument("--config", action="append", default=[],
                   help="fleet config YAML; repeat to test several fleets")
    p.add_argument("--fleet-target", action="append", default=[], metavar="FLEET=WAYPOINT",
                   help="waypoint one fleet must reach (with --config)")
    p.add_argument("--nav-graph", help="nav graph YAML (with --config)")
    p.add_argument("--allow-remote", action="store_true",
                   help="allow a broker that is not localhost (with --config)")
    args = p.parse_args()

    if args.config:
        return run_fleets(args)
    args.host = args.host or "localhost"
    args.port = args.port or 1883

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
