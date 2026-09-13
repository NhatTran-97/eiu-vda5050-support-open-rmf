#!/usr/bin/env python3
"""
Minimal RMF delivery dispatcher (no rmf_demos needed).

Publishes a dispatch_task_request ApiRequest to /task_api_requests, same
mechanism as dispatch_patrol.py -- see that script for the QoS/handshake notes.

    python3 dispatch_delivery.py Patrol_A1 mock_dispenser_1 Patrol_F1 mock_ingestor_1
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
    parser = argparse.ArgumentParser(description="Dispatch an RMF delivery task.")
    parser.add_argument("pickup_place", help="waypoint to pick up at")
    parser.add_argument("pickup_handler", help="dispenser guid at pickup_place")
    parser.add_argument("dropoff_place", help="waypoint to drop off at")
    parser.add_argument("dropoff_handler", help="ingestor guid at dropoff_place")
    parser.add_argument("--sku", default="box", help="payload sku (default 'box')")
    parser.add_argument("--quantity", type=int, default=1)
    parser.add_argument("--fleet", default="", help="optional fleet name to target")
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("rmf_cli_dispatch_delivery")

    qos = QoSProfile(
        history=QoSHistoryPolicy.KEEP_LAST,
        depth=10,
        reliability=QoSReliabilityPolicy.RELIABLE,
        durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
    )
    pub = node.create_publisher(ApiRequest, "/task_api_requests", qos)

    payload = {"sku": args.sku, "quantity": args.quantity}
    request = {
        "category": "delivery",
        "description": {
            "pickup": {"place": args.pickup_place, "handler": args.pickup_handler,
                      "payload": payload},
            "dropoff": {"place": args.dropoff_place, "handler": args.dropoff_handler,
                       "payload": payload},
        },
        "unix_millis_earliest_start_time": 0,
        "requester": "cli",
    }
    if args.fleet:
        request["fleet_name"] = args.fleet

    msg = ApiRequest()
    msg.request_id = "cli-" + uuid.uuid4().hex[:8]
    msg.json_msg = json.dumps({"type": "dispatch_task_request", "request": request})

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
    print(f"Dispatched delivery -> {args.pickup_place} ({args.pickup_handler}) -> "
          f"{args.dropoff_place} ({args.dropoff_handler}) (request_id={msg.request_id})")

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
