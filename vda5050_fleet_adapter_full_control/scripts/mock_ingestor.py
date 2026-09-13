#!/usr/bin/env python3
"""
mock_ingestor.py — simulate an RMF ingestor workcell.

TurtleBot3 has no drop/unload hardware, so this stands in for one: it answers
IngestorRequest on /ingestor_requests with an ACKNOWLEDGED result, waits
--unload-time seconds (simulating the physical unload), then a SUCCESS result.
Publishes IngestorState at 1 Hz so RMF's IngestItem phase has both signals
it looks for (see rmf_fleet_adapter's IngestItem::ActivePhase).

Usage:
  python3 mock_ingestor.py --guid mock_ingestor_1
  python3 mock_ingestor.py --guid mock_ingestor_1 --unload-time 5.0
"""
import argparse
import sys

import rclpy
from rclpy.node import Node
from rclpy.utilities import remove_ros_args
from rmf_ingestor_msgs.msg import IngestorRequest, IngestorResult, IngestorState


class MockIngestor(Node):
    def __init__(self, guid: str, unload_time: float):
        super().__init__(f"mock_ingestor_{guid}")
        self.guid = guid
        self.unload_time = unload_time
        self.queue: list[str] = []

        self.result_pub = self.create_publisher(IngestorResult, "/ingestor_results", 10)
        self.state_pub = self.create_publisher(IngestorState, "/ingestor_states", 10)
        self.create_subscription(IngestorRequest, "/ingestor_requests", self._on_request, 10)
        self.create_timer(1.0, self._publish_state)

    def _on_request(self, msg: IngestorRequest):
        if msg.target_guid != self.guid or msg.request_guid in self.queue:
            return

        self.get_logger().info(f"ingesting for request {msg.request_guid}")
        self.queue.append(msg.request_guid)
        self._publish_result(msg.request_guid, IngestorResult.ACKNOWLEDGED)

        timer = None
        def finish():
            timer.cancel()
            self._finish(msg.request_guid)
        timer = self.create_timer(self.unload_time, finish)

    def _finish(self, request_guid: str):
        if request_guid not in self.queue:
            return
        self.queue.remove(request_guid)
        self._publish_result(request_guid, IngestorResult.SUCCESS)
        self.get_logger().info(f"ingested for request {request_guid}")

    def _publish_result(self, request_guid: str, status: int):
        msg = IngestorResult()
        msg.time = self.get_clock().now().to_msg()
        msg.request_guid = request_guid
        msg.source_guid = self.guid
        msg.status = status
        self.result_pub.publish(msg)

    def _publish_state(self):
        msg = IngestorState()
        msg.time = self.get_clock().now().to_msg()
        msg.guid = self.guid
        msg.mode = IngestorState.BUSY if self.queue else IngestorState.IDLE
        msg.request_guid_queue = list(self.queue)
        self.state_pub.publish(msg)


def main():
    parser = argparse.ArgumentParser(description="Mock RMF ingestor workcell.")
    parser.add_argument("--guid", default="mock_ingestor_1",
                        help="workcell name -- matches the delivery task's dropoff.handler")
    parser.add_argument("--unload-time", type=float, default=5.0,
                        help="seconds to simulate unloading (default 5.0)")
    args = parser.parse_args(remove_ros_args(args=sys.argv)[1:])

    rclpy.init()
    node = MockIngestor(args.guid, args.unload_time)
    print(f"[mock_ingestor] guid={args.guid} unload_time={args.unload_time}s")
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
