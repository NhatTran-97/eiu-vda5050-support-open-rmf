#!/usr/bin/env python3
"""
mock_dispenser.py — simulate an RMF dispenser workcell.

TurtleBot3 has no pick/load hardware, so this stands in for one: it answers
DispenserRequest on /dispenser_requests with an ACKNOWLEDGED result, waits
--load-time seconds (simulating the physical load), then a SUCCESS result.
Publishes DispenserState at 1 Hz so RMF's DispenseItem phase has both signals
it looks for (see rmf_fleet_adapter's DispenseItem::ActivePhase).

Usage:
  python3 mock_dispenser.py --guid mock_dispenser_1
  python3 mock_dispenser.py --guid mock_dispenser_1 --load-time 5.0
"""
import argparse
import sys

import rclpy
from rclpy.node import Node
from rclpy.utilities import remove_ros_args
from rmf_dispenser_msgs.msg import DispenserRequest, DispenserResult, DispenserState


class MockDispenser(Node):
    def __init__(self, guid: str, load_time: float):
        super().__init__(f"mock_dispenser_{guid}")
        self.guid = guid
        self.load_time = load_time
        self.queue: list[str] = []

        self.result_pub = self.create_publisher(DispenserResult, "/dispenser_results", 10)
        self.state_pub = self.create_publisher(DispenserState, "/dispenser_states", 10)
        self.create_subscription(DispenserRequest, "/dispenser_requests", self._on_request, 10)
        self.create_timer(1.0, self._publish_state)

    def _on_request(self, msg: DispenserRequest):
        if msg.target_guid != self.guid or msg.request_guid in self.queue:
            return

        self.get_logger().info(f"dispensing for request {msg.request_guid}")
        self.queue.append(msg.request_guid)
        self._publish_result(msg.request_guid, DispenserResult.ACKNOWLEDGED)

        timer = None
        def finish():
            timer.cancel()
            self._finish(msg.request_guid)
        timer = self.create_timer(self.load_time, finish)

    def _finish(self, request_guid: str):
        if request_guid not in self.queue:
            return
        self.queue.remove(request_guid)
        self._publish_result(request_guid, DispenserResult.SUCCESS)
        self.get_logger().info(f"dispensed for request {request_guid}")

    def _publish_result(self, request_guid: str, status: int):
        msg = DispenserResult()
        msg.time = self.get_clock().now().to_msg()
        msg.request_guid = request_guid
        msg.source_guid = self.guid
        msg.status = status
        self.result_pub.publish(msg)

    def _publish_state(self):
        msg = DispenserState()
        msg.time = self.get_clock().now().to_msg()
        msg.guid = self.guid
        msg.mode = DispenserState.BUSY if self.queue else DispenserState.IDLE
        msg.request_guid_queue = list(self.queue)
        self.state_pub.publish(msg)


def main():
    parser = argparse.ArgumentParser(description="Mock RMF dispenser workcell.")
    parser.add_argument("--guid", default="mock_dispenser_1",
                        help="workcell name -- matches the delivery task's pickup.handler")
    parser.add_argument("--load-time", type=float, default=5.0,
                        help="seconds to simulate loading (default 5.0)")
    args = parser.parse_args(remove_ros_args(args=sys.argv)[1:])

    rclpy.init()
    node = MockDispenser(args.guid, args.load_time)
    print(f"[mock_dispenser] guid={args.guid} load_time={args.load_time}s")
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
