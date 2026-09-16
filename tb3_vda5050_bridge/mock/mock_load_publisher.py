#!/usr/bin/env python3
"""Publish a VDA5050 Load, standing in for a load sensor."""
import argparse
import sys

import rclpy
from rclpy.node import Node
from rclpy.utilities import remove_ros_args
from vda5050_msgs.msg import Load

LOAD_TOPIC = "/vda5050_client_adapter/load"


class MockLoadPublisher(Node):
    def __init__(self, load_id: str, load_type: str, weight: float):
        super().__init__("mock_load_publisher")
        self.pub = self.create_publisher(Load, LOAD_TOPIC, 10)
        self.msg = Load()
        self.msg.load_id = load_id
        self.msg.load_type = load_type
        self.msg.weight = weight
        self.create_timer(1.0, self._publish)

    def _publish(self):
        self.pub.publish(self.msg)


def main():
    parser = argparse.ArgumentParser(description="Publish a VDA5050 Load repeatedly, like a real sensor would.")
    parser.add_argument("--load-id", default="box-01")
    parser.add_argument("--load-type", default="box")
    parser.add_argument("--weight", type=float, default=20.0, help="kg")
    args = parser.parse_args(remove_ros_args(args=sys.argv)[1:])

    rclpy.init()
    node = MockLoadPublisher(args.load_id, args.load_type, args.weight)
    print(f"[mock_load_publisher] load_id={args.load_id} load_type={args.load_type} "
          f"weight={args.weight}kg -> {LOAD_TOPIC} every 1s")
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
