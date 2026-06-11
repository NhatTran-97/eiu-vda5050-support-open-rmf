#!/usr/bin/env python3
"""
Publish nav_graph.yaml waypoints and lanes as RViz MarkerArray.
Usage:
  ros2 run vda5050_fleet_adapter visualize_nav_graph.py \
      --ros-args -p nav_graph_path:=<path_to_nav_graph.yaml>
"""

import rclpy
from rclpy.node import Node
from visualization_msgs.msg import Marker, MarkerArray
from geometry_msgs.msg import Point
from std_msgs.msg import ColorRGBA
import yaml
import math


class NavGraphVisualizer(Node):
    def __init__(self):
        super().__init__('nav_graph_visualizer')

        self.declare_parameter(
            'nav_graph_path',
            '/home/duynhat/ros2_ws/install/vda5050_fleet_adapter/'
            'share/vda5050_fleet_adapter/maps/nav_graph.yaml')
        self.declare_parameter('frame_id', 'map')
        self.declare_parameter('publish_rate', 1.0)

        path = self.get_parameter('nav_graph_path').value
        self._frame = self.get_parameter('frame_id').value
        rate = self.get_parameter('publish_rate').value

        self._pub = self.create_publisher(MarkerArray, '/nav_graph_markers', 10)
        self._markers = self._load(path)

        self.create_timer(1.0 / rate, self._publish)
        self.get_logger().info(f'Visualizing {path} on frame [{self._frame}]')

    def _load(self, path: str) -> MarkerArray:
        with open(path) as f:
            data = yaml.safe_load(f)

        level = next(iter(data['levels'].values()))
        vertices = level.get('vertices', [])
        lanes    = level.get('lanes', [])

        markers = MarkerArray()
        mid = 0

        # ── spheres + labels ────────────────────────────────────────────────
        for idx, v in enumerate(vertices):
            x, y = float(v[0]), float(v[1])
            props = v[2] if len(v) > 2 else {}
            name  = props.get('name', f'wp{idx}')
            is_charger  = bool(props.get('is_charger', False))
            is_parking  = bool(props.get('is_parking_spot', False))

            # Color: charger=yellow, parking=cyan, normal=white
            if is_charger:
                color = ColorRGBA(r=1.0, g=1.0, b=0.0, a=0.9)
            elif is_parking:
                color = ColorRGBA(r=0.0, g=1.0, b=1.0, a=0.9)
            else:
                color = ColorRGBA(r=1.0, g=1.0, b=1.0, a=0.8)

            # Sphere
            m = Marker()
            m.header.frame_id = self._frame
            m.ns   = 'waypoints'
            m.id   = mid; mid += 1
            m.type = Marker.SPHERE
            m.action = Marker.ADD
            m.pose.position.x = x
            m.pose.position.y = y
            m.pose.position.z = 0.0
            m.pose.orientation.w = 1.0
            m.scale.x = m.scale.y = m.scale.z = 0.25
            m.color = color
            markers.markers.append(m)

            # Label
            t = Marker()
            t.header.frame_id = self._frame
            t.ns   = 'labels'
            t.id   = mid; mid += 1
            t.type = Marker.TEXT_VIEW_FACING
            t.action = Marker.ADD
            t.pose.position.x = x + 0.15
            t.pose.position.y = y + 0.15
            t.pose.position.z = 0.35
            t.pose.orientation.w = 1.0
            t.scale.z = 0.22
            t.color = ColorRGBA(r=0.0, g=0.0, b=0.0, a=1.0)
            suffix = ''
            if is_charger:  suffix += ' ⚡'
            if is_parking:  suffix += ' P'
            t.text = f'[{idx}] {name}{suffix}'
            markers.markers.append(t)

        # ── lanes as lines ──────────────────────────────────────────────────
        for lane in lanes:
            a, b = int(lane[0]), int(lane[1])
            if a >= len(vertices) or b >= len(vertices):
                continue
            x0, y0 = float(vertices[a][0]), float(vertices[a][1])
            x1, y1 = float(vertices[b][0]), float(vertices[b][1])

            line = Marker()
            line.header.frame_id = self._frame
            line.ns   = 'lanes'
            line.id   = mid; mid += 1
            line.type = Marker.ARROW
            line.action = Marker.ADD
            line.points = [
                Point(x=x0, y=y0, z=0.05),
                Point(x=x1, y=y1, z=0.05),
            ]
            line.scale.x = 0.08   # shaft diameter
            line.scale.y = 0.18   # head diameter
            line.scale.z = 0.15   # head length
            line.color = ColorRGBA(r=0.0, g=0.9, b=0.2, a=0.9)
            markers.markers.append(line)

        self.get_logger().info(
            f'Loaded {len(vertices)} waypoints, {len(lanes)} lanes')
        return markers

    def _publish(self):
        now = self.get_clock().now().to_msg()
        for m in self._markers.markers:
            m.header.stamp = now
        self._pub.publish(self._markers)


def main():
    rclpy.init()
    node = NavGraphVisualizer()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
