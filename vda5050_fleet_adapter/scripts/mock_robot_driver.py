#!/usr/bin/env python3
"""
Mock Robot Driver — simulates a real robot responding to VDA5050 orders and actions.
"""

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile
import threading
import time

from std_msgs.msg import Bool, String
from vda5050_msgs.msg import (
    Order, NodeState, EdgeState, AgvPosition,
    Action, ActionState
)


class MockRobotDriver(Node):
    def __init__(self):
        super().__init__('mock_robot_driver')

        qos = QoSProfile(depth=10)

        # ── Subscribers ──────────────────────────────────────────────────────
        self.create_subscription(Order,  'order',          self._on_order,         qos)
        self.create_subscription(Action, 'action_execute', self._on_action_execute, qos)
        self.create_subscription(String, 'action_cancel',  self._on_action_cancel,  qos)

        # ── Publishers ───────────────────────────────────────────────────────
        self._pub_driving   = self.create_publisher(Bool,        'driving',              qos)
        self._pub_reached   = self.create_publisher(NodeState,   'node_reached',         qos)
        self._pub_entered   = self.create_publisher(EdgeState,   'edge_entered',         qos)
        self._pub_completed = self.create_publisher(EdgeState,   'edge_completed',       qos)
        self._pub_position  = self.create_publisher(AgvPosition, 'agv_position',         qos)
        self._pub_paused    = self.create_publisher(Bool,        'paused',               qos)
        self._pub_feedback  = self.create_publisher(ActionState, 'action_state_feedback', qos)

        self._nav_thread  = None
        self._cancel_flag = threading.Event()
        self._paused      = False

        self.get_logger().info('Mock Robot Driver started — waiting for orders...')

    # ── Order callback ────────────────────────────────────────────────────────

    def _on_order(self, msg: Order):
        self.get_logger().info(
            f'Order received: id={msg.order_id} '
            f'nodes={len(msg.nodes)} edges={len(msg.edges)}')

        self._cancel_flag.set()
        if self._nav_thread and self._nav_thread.is_alive():
            self._nav_thread.join(timeout=2.0)

        self._cancel_flag.clear()
        self._paused = False
        self._publish_paused(False)

        self._nav_thread = threading.Thread(
            target=self._simulate_navigation,
            args=(msg,), daemon=True)
        self._nav_thread.start()

    # ── Action callbacks ──────────────────────────────────────────────────────

    def _on_action_execute(self, msg: Action):
        self.get_logger().info(f'Action: type={msg.action_type} id={msg.action_id}')

        def handle():
            self._send_action_state(msg.action_id, msg.action_type, 'RUNNING')
            time.sleep(0.5)

            if msg.action_type == 'startPause':
                self._paused = True
                self._publish_paused(True)
                self._publish_driving(False)

            elif msg.action_type == 'stopPause':
                self._paused = False
                self._publish_paused(False)

            elif msg.action_type == 'stateRequest':
                pass  # state published automatically

            self._send_action_state(msg.action_id, msg.action_type, 'FINISHED')

        threading.Thread(target=handle, daemon=True).start()

    def _on_action_cancel(self, msg: String):
        self.get_logger().info(f'Action cancel: {msg.data}')

    # ── Navigation simulation ─────────────────────────────────────────────────

    def _simulate_navigation(self, order: Order):
        self.get_logger().info(f'Navigating through {len(order.nodes)} nodes...')
        self._publish_driving(True)

        for i, node in enumerate(order.nodes):
            if self._cancel_flag.is_set():
                self._publish_driving(False)
                return

            # Wait while paused
            while self._paused and not self._cancel_flag.is_set():
                time.sleep(0.2)

            if i > 0:
                self.get_logger().info(
                    f'  Moving to [{node.node_id}] '
                    f'({node.node_position.x:.1f}, {node.node_position.y:.1f})...')

                if i - 1 < len(order.edges):
                    self._publish_edge_entered(order.edges[i - 1])

                time.sleep(2.0)

                if self._cancel_flag.is_set():
                    self._publish_driving(False)
                    return

                if i - 1 < len(order.edges):
                    self._publish_edge_completed(order.edges[i - 1])

            self._publish_position(
                node.node_position.x,
                node.node_position.y,
                node.node_position.theta,
                node.node_position.map_id or 'L1')

            self._publish_node_reached(node)
            self.get_logger().info(f'  Arrived at [{node.node_id}]')

        self._publish_driving(False)
        self.get_logger().info(f'Order [{order.order_id}] DONE!')

    # ── Publish helpers ───────────────────────────────────────────────────────

    def _publish_driving(self, val: bool):
        m = Bool(); m.data = val; self._pub_driving.publish(m)

    def _publish_paused(self, val: bool):
        m = Bool(); m.data = val; self._pub_paused.publish(m)

    def _publish_node_reached(self, node):
        m = NodeState()
        m.node_id = node.node_id
        m.sequence_id = node.sequence_id
        m.released = node.released
        if node.node_position:
            m.node_position = node.node_position
            m.node_position_set = True
        self._pub_reached.publish(m)

    def _publish_edge_entered(self, edge):
        m = EdgeState()
        m.edge_id = edge.edge_id
        m.sequence_id = edge.sequence_id
        m.released = edge.released
        self._pub_entered.publish(m)

    def _publish_edge_completed(self, edge):
        m = EdgeState()
        m.edge_id = edge.edge_id
        m.sequence_id = edge.sequence_id
        m.released = edge.released
        self._pub_completed.publish(m)

    def _publish_position(self, x, y, theta, map_id):
        m = AgvPosition()
        m.x = x; m.y = y; m.theta = theta
        m.map_id = map_id
        m.position_initialized = True
        m.localization_score = 1.0
        self._pub_position.publish(m)

    def _send_action_state(self, action_id, action_type, status):
        m = ActionState()
        m.action_id = action_id
        m.action_type = action_type
        m.action_status = status
        self._pub_feedback.publish(m)


def main():
    rclpy.init()
    node = MockRobotDriver()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
