#!/usr/bin/env python3
"""Submit an RMF patrol task."""
import argparse
import json
import sys
import time
import uuid

import rclpy
from rclpy.qos import (QoSProfile, QoSDurabilityPolicy,
                       QoSReliabilityPolicy, QoSHistoryPolicy)
from rmf_task_msgs.msg import ApiRequest, ApiResponse


def main():
    parser = argparse.ArgumentParser(description="Dispatch an RMF patrol task.")
    parser.add_argument("places", nargs="+", help="waypoint names to visit")
    parser.add_argument("-n", "--rounds", type=int, default=1,
                        help="number of loops over the places (default 1)")
    parser.add_argument("--fleet", default="", help="optional fleet name to target")
    parser.add_argument("--robot", default="",
                        help="assign the task to this robot of --fleet instead of letting RMF choose")
    args = parser.parse_args()
    if args.robot and not args.fleet:
        parser.error("--robot needs --fleet")

    rclpy.init()
    node = rclpy.create_node("rmf_cli_dispatch")

    qos = QoSProfile(
        history=QoSHistoryPolicy.KEEP_LAST,
        depth=10,
        reliability=QoSReliabilityPolicy.RELIABLE,
        durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
    )
    pub = node.create_publisher(ApiRequest, "/task_api_requests", qos)

    request = {
        "category": "patrol",
        "description": {"places": args.places, "rounds": args.rounds},
        "unix_millis_earliest_start_time": 0,
        "requester": "cli",
    }
    if args.robot:
        envelope = {"type": "robot_task_request", "robot": args.robot, "fleet": args.fleet, "request": request}
    else:
        if args.fleet:
            request["fleet_name"] = args.fleet
        envelope = {"type": "dispatch_task_request", "request": request}

    msg = ApiRequest()
    msg.request_id = "cli-" + uuid.uuid4().hex[:8]
    msg.json_msg = json.dumps(envelope)

    # Match RMF's response to the requested task ID.
    result = {"task_id": None}

    def on_response(resp):
        if resp.request_id != msg.request_id:
            return
        try:
            data = json.loads(resp.json_msg)
            result["task_id"] = data.get("state", {}).get("booking", {}).get("id")
        except Exception:
            pass

    node.create_subscription(ApiResponse, "/task_api_responses", on_response, qos)

    # Wait until the dispatcher subscribes to task requests.
    print("Waiting for /task_api_requests subscriber (the RMF dispatcher)...")
    for _ in range(50):
        if pub.get_subscription_count() > 0:
            break
        rclpy.spin_once(node, timeout_sec=0.1)
        time.sleep(0.1)
    else:
        print("ERROR: no subscriber on /task_api_requests. Check that the "
              "dispatcher is running AND this terminal uses the same "
              "ROS_DOMAIN_ID as the RMF core.", file=sys.stderr)
        node.destroy_node()
        rclpy.shutdown()
        sys.exit(1)

    pub.publish(msg)
    print(f"Dispatched patrol -> places={args.places} rounds={args.rounds} "
          f"(request_id={msg.request_id})")

    # Receive the assigned RMF task ID after publishing.
    end = time.time() + 3.0
    while time.time() < end and result["task_id"] is None:
        rclpy.spin_once(node, timeout_sec=0.1)

    if result["task_id"]:
        print(f"task_id = {result['task_id']}")
        print(f"Cancel with:  python3 cancel_task.py {result['task_id']}")
    else:
        print("(task_id not received; check 'ros2 topic echo /task_api_responses')")

    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
