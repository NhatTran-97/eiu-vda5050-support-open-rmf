#!/usr/bin/env python3
"""
Add or remove a robot of a running fleet adapter, and list what it knows.

Talks to the adapter over the JSON topics the dashboard uses:
  /robot_registration_requests  ->  add / remove request
  /robot_registration_results   <-  verdict with errors and warnings
  /robot_registry               <-  robots and chargers per fleet (latched)
  /robot_discovery              <-  robots seen on the broker but not registered, one snapshot per fleet (latched)
"""
import argparse
import json
import sys
import time
import uuid

import rclpy
from rclpy.qos import QoSDurabilityPolicy, QoSProfile, QoSReliabilityPolicy
from std_msgs.msg import String

REQUEST_TOPIC = "/robot_registration_requests"
RESULT_TOPIC = "/robot_registration_results"
REGISTRY_TOPIC = "/robot_registry"
DISCOVERY_TOPIC = "/robot_discovery"

EXIT_OK, EXIT_REFUSED, EXIT_NO_REPLY = 0, 1, 2

# Seconds between resends of a request that has not been answered.
RESEND_PERIOD = 1.5


def latched_qos() -> QoSProfile:
    return QoSProfile(depth=20, reliability=QoSReliabilityPolicy.RELIABLE,
                      durability=QoSDurabilityPolicy.TRANSIENT_LOCAL)


def volatile_qos() -> QoSProfile:
    return QoSProfile(depth=20, reliability=QoSReliabilityPolicy.RELIABLE)


def spin_for(node, seconds: float, until=None):
    """Spin the node for `seconds`, or until `until()` becomes true."""
    end = time.monotonic() + seconds
    while time.monotonic() < end and not (until and until()):
        rclpy.spin_once(node, timeout_sec=0.1)


def collect_latched(node, topic: str, seconds: float) -> list:
    """Read the messages a latched topic replays to a new subscriber."""
    messages = []

    def on_message(msg):
        try:
            messages.append(json.loads(msg.data))
        except ValueError:
            pass

    node.create_subscription(String, topic, on_message, latched_qos())
    spin_for(node, seconds)
    return messages


def print_findings(result: dict):
    for e in result.get("errors", []):
        print(f"  ERROR    {e['code']}: {e['message']}")
    for w in result.get("warnings", []):
        print(f"  WARNING  {w['code']}: {w['message']}")


def send_request(node, request: dict, timeout: float) -> int:
    request_id = "cli-" + uuid.uuid4().hex[:8]
    request["request_id"] = request_id

    replies = []

    def on_result(msg):
        try:
            data = json.loads(msg.data)
        except ValueError:
            return
        if data.get("request_id") == request_id:
            replies.append(data)

    publisher = node.create_publisher(String, REQUEST_TOPIC, volatile_qos())
    node.create_subscription(String, RESULT_TOPIC, on_result, volatile_qos())

    # Wait until an adapter is listening; otherwise the request would be lost.
    spin_for(node, 5.0, until=lambda: publisher.get_subscription_count() > 0)
    if publisher.get_subscription_count() == 0:
        print(f"No adapter is listening on {REQUEST_TOPIC} (same ROS_DOMAIN_ID as the fleet adapter?).",
              file=sys.stderr)
        return EXIT_NO_REPLY

    # Resend until answered: a new process may not be matched with every adapter yet. A repeated request_id gets the same result.
    end = time.monotonic() + timeout
    next_send = 0.0
    while time.monotonic() < end and not replies:
        if time.monotonic() >= next_send:
            publisher.publish(String(data=json.dumps(request)))
            next_send = time.monotonic() + RESEND_PERIOD
        rclpy.spin_once(node, timeout_sec=0.1)
    if not replies:
        print(f"No reply within {timeout:.0f}s: is '{request['fleet']}' the fleet's exact name?", file=sys.stderr)
        return EXIT_NO_REPLY

    reply = replies[0]
    label = f"{reply['action']} {reply['name']}"
    if reply["ok"] and reply.get("dry_run"):
        print(f"[{label}] checks passed; nothing was added")
        print_findings(reply)
        return EXIT_OK
    if reply["ok"]:
        saved = "saved for the next start" if reply.get("persisted") else "NOT saved for the next start"
        print(f"[{label}] OK, {saved}")
        print_findings(reply)
        return EXIT_OK

    print(f"[{label}] {'would be REFUSED' if reply.get('dry_run') else 'REFUSED'}")
    print_findings(reply)
    if reply.get("needs_confirmation"):
        print("  Only unverifiable checks are in the way; repeat with --confirm-unverified to add it anyway.")
    return EXIT_REFUSED


def show_registry(node):
    registries = collect_latched(node, REGISTRY_TOPIC, 2.0)
    if not registries:
        print("No fleet adapter published a registry.")
        return
    latest = {}
    for r in registries:
        latest[r["fleet"]] = r
    for fleet, r in sorted(latest.items()):
        limits = r["limits"]
        print(f"Fleet {fleet} (interface {r['interface']}, type {r.get('series') or 'unknown'}): "
              f"footprint radius {limits['footprint_radius']:.2f} m, speed {limits['linear_speed']:.2f} m/s, "
              f"acceleration {limits['linear_acceleration']:.2f} m/s^2, tolerance {limits.get('tolerance', 0.0):.0%}")
        for robot in r["robots"]:
            state = "removed" if robot["retired"] else robot["source"]
            print(f"  robot   {robot['name']:<16} {robot['manufacturer']}/{robot['serial']:<10} "
                  f"charger {robot['charger'] or '-':<12} [{state}]")
        for charger in r["chargers"]:
            user = charger["used_by"]
            note = f"used by {user}" + (" (removed)" if charger.get("used_by_removed") else "") if user else "free"
            print(f"  charger {charger['name']:<16} {note}")


def show_discovery(node):
    """Merge the pending-robot snapshots that each fleet adapter published."""
    seen = {}
    for snapshot in collect_latched(node, DISCOVERY_TOPIC, 2.0):
        for robot in snapshot.get("robots", []):
            key = (robot["manufacturer"], robot["serial"])
            kept, reporters = seen.setdefault(key, (robot, []))
            reporters.append(snapshot["reporter"])
            # The fleet that removed the robot knows what it was called; the others only see an unknown robot.
            if robot.get("removed_as") and not kept.get("removed_as"):
                seen[key] = (robot, reporters)
    if not seen:
        print("No unregistered robot has been reported.")
        return
    for (manufacturer, serial), (robot, reporters) in sorted(seen.items()):
        pose = robot.get("pose")
        where = (f"at ({pose['x']:.2f}, {pose['y']:.2f}) on '{pose['map']}'"
                 f"{'' if pose['initialized'] else ' (not localized)'}") if pose else "no state yet"
        kind = robot.get("series") or "no factsheet"
        removed = robot.get("removed_as")
        if removed:
            kind = f"removed, was '{removed['name']}' in fleet '{removed['fleet']}' at {removed['charger']}"
        print(f"  {manufacturer}/{serial}: {kind}, {where}  (seen by {', '.join(sorted(reporters))})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    add = sub.add_parser("add", help="register a robot in a fleet")
    add.add_argument("--fleet", required=True)
    add.add_argument("--name", required=True)
    add.add_argument("--manufacturer", required=True)
    add.add_argument("--serial", required=True)
    add.add_argument("--charger", required=True)
    add.add_argument("--responsive-wait", action="store_true")
    add.add_argument("--rotation", type=float)
    add.add_argument("--scale", type=float)
    add.add_argument("--translation", type=float, nargs=2, metavar=("X", "Y"))
    add.add_argument("--confirm-unverified", action="store_true",
                     help="add the robot even if some checks could not be made")
    add.add_argument("--check", action="store_true", help="only run the checks; add nothing")

    remove = sub.add_parser("remove", help="decommission a runtime-added robot and stop tracking it")
    remove.add_argument("--fleet", required=True)
    remove.add_argument("--name", required=True)

    sub.add_parser("list", help="show every fleet's robots and chargers")
    sub.add_parser("discovered", help="show robots seen on the broker that no fleet has registered")

    parser.add_argument("--timeout", type=float, default=10.0, help="seconds to wait for the reply")
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("register_robot_cli")
    try:
        if args.command == "list":
            show_registry(node)
            return EXIT_OK
        if args.command == "discovered":
            show_discovery(node)
            return EXIT_OK

        if args.command == "add":
            request = {"action": "add", "fleet": args.fleet, "name": args.name,
                       "manufacturer": args.manufacturer, "serial": args.serial, "charger": args.charger,
                       "responsive_wait": args.responsive_wait, "confirm_unverified": args.confirm_unverified,
                       "dry_run": args.check}
            if args.rotation is not None or args.scale is not None or args.translation is not None:
                transform = {"rotation": args.rotation if args.rotation is not None else 0.0,
                             "scale": args.scale if args.scale is not None else 1.0}
                if args.translation is not None:
                    transform["translation"] = list(args.translation)
                request["transform"] = transform
        else:
            request = {"action": "remove", "fleet": args.fleet, "name": args.name}
        return send_request(node, request, args.timeout)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    sys.exit(main())
