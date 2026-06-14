#!/usr/bin/env python3
"""
Cancel a running RMF task by task_id.

The task_id is printed by dispatch_patrol.py, or read from
    ros2 topic echo /task_api_responses

Usage (run on the HOST, same ROS_DOMAIN_ID as the RMF core — domain 5 here):
    python3 cancel_task.py patrol.dispatch-3
"""
import argparse
import json
import sys
import time
import uuid

import rclpy
from rclpy.qos import (QoSProfile, QoSDurabilityPolicy,
                       QoSReliabilityPolicy, QoSHistoryPolicy)
from rmf_task_msgs.msg import ApiRequest


def main():
    parser = argparse.ArgumentParser(description="Cancel an RMF task by id.")
    parser.add_argument("task_id", help="task_id to cancel (e.g. patrol.dispatch-3)")
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("rmf_cli_cancel")

    qos = QoSProfile(
        history=QoSHistoryPolicy.KEEP_LAST,
        depth=10,
        reliability=QoSReliabilityPolicy.RELIABLE,
        durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
    )
    pub = node.create_publisher(ApiRequest, "/task_api_requests", qos)

    msg = ApiRequest()
    msg.request_id = "cancel-" + uuid.uuid4().hex[:8]
    msg.json_msg = json.dumps({"type": "cancel_task_request", "task_id": args.task_id})

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
    print(f"Cancel request sent for task_id={args.task_id} (request_id={msg.request_id})")

    end = time.time() + 1.0
    while time.time() < end:
        rclpy.spin_once(node, timeout_sec=0.1)

    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
