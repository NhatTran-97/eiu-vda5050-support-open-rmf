#!/usr/bin/env python3
"""
Minimal RMF patrol dispatcher (no rmf_demos needed).

Publishes a dispatch_task_request ApiRequest to /task_api_requests with the QoS
the RMF dispatcher expects (reliable + transient_local), then waits for the
subscription so the request is actually delivered.

    python3 dispatch_patrol.py wp2_parking
    python3 dispatch_patrol.py wp2_parking wp1_charging initial_wp --rounds 2
    python3 ~/ros2_ws/src/vda5050_fleet_adapter/scripts/dispatch_patrol.py wp6

"""
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
    args = parser.parse_args()

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
    if args.fleet:
        request["fleet_name"] = args.fleet

    msg = ApiRequest()
    msg.request_id = "cli-" + uuid.uuid4().hex[:8]
    msg.json_msg = json.dumps({"type": "dispatch_task_request", "request": request})

    # Capture the task_id RMF assigns, from the matching response.
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

    # Wait for the dispatcher's subscription so the message is delivered.
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

    # Spin to let the request go out and the response (with task_id) come back.
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
