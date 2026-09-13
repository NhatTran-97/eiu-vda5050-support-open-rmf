#!/usr/bin/env python3
"""Simulate a VDA5050 robot over MQTT for adapter testing."""
import argparse
import datetime
import json
import threading
import time

import paho.mqtt.client as mqtt


def iso_now() -> str:
    now = datetime.datetime.now(datetime.timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


class MockRobot:
    def __init__(self, args):
        self.args = args
        self.base = f"{args.interface}/v2/{args.manufacturer}/{args.serial}"

        self.x, self.y, self.theta = args.x, args.y, args.theta
        self.map_id = args.map
        self.last_node_id = args.start_node
        self.last_node_sequence_id = 0
        self.order_id = ""
        self.order_update_id = 0
        self.driving = False
        # Whether the robot is waiting for more released route points.
        self.new_base_request = False
        self.node_states = []
        self.edge_states = []
        self.action_states = []
        self.header = 0

        # Full route arrays, including points still in the horizon.
        self.nodes = []
        self.edges = []

        self.lock = threading.Lock()
        # Wake the drive loop when a new horizon arrives.
        self.cond = threading.Condition(self.lock)
        self._drive_thread = None
        self._cancel = threading.Event()
        # Hold the active order while paused.
        self.paused = threading.Event()

        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        # Mark the robot offline if the MQTT connection drops.
        self.client.will_set(
            f"{self.base}/connection",
            json.dumps(self._connection_payload("CONNECTIONBROKEN")),
            qos=1, retain=True)

    # MQTT connection and callbacks.

    def _next_header(self) -> int:
        self.header += 1
        return self.header

    def _on_connect(self, client, userdata, flags, rc):
        client.subscribe(f"{self.base}/order", qos=1)
        client.subscribe(f"{self.base}/instantActions", qos=1)
        self.publish_connection("ONLINE")
        self.publish_state()
        print(f"[mock] connected — identity {self.base}", flush=True)

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
        except Exception:
            return
        if msg.topic.endswith("/order"):
            self._handle_order(payload)
        elif msg.topic.endswith("/instantActions"):
            self._handle_instant_actions(payload)

    # Publish VDA5050 messages.

    def _connection_payload(self, state: str) -> dict:
        return {
            "headerId": self._next_header(),
            "timestamp": iso_now(),
            "version": "2.1.0",
            "manufacturer": self.args.manufacturer,
            "serialNumber": self.args.serial,
            "connectionState": state,
        }

    def publish_connection(self, state: str):
        self.client.publish(f"{self.base}/connection",
                            json.dumps(self._connection_payload(state)),
                            qos=1, retain=True)

    def publish_state(self):
        with self.lock:
            msg = {
                "headerId": self._next_header(),
                "timestamp": iso_now(),
                "version": "2.1.0",
                "manufacturer": self.args.manufacturer,
                "serialNumber": self.args.serial,
                "orderId": self.order_id,
                "orderUpdateId": self.order_update_id,
                "lastNodeId": self.last_node_id,
                "lastNodeSequenceId": self.last_node_sequence_id,
                "nodeStates": list(self.node_states),
                "edgeStates": list(self.edge_states),
                "actionStates": list(self.action_states),
                "driving": self.driving,
                "paused": self.paused.is_set(),
                "newBaseRequest": self.new_base_request,
                "operatingMode": "AUTOMATIC",
                "batteryState": {"batteryCharge": 95.0, "charging": False},
                "agvPosition": {
                    "x": self.x, "y": self.y, "theta": self.theta,
                    "mapId": self.map_id, "positionInitialized": True,
                },
                "errors": [], "information": [],
                "safetyState": {"eStop": "NONE", "fieldViolation": False},
            }
        self.client.publish(f"{self.base}/state", json.dumps(msg), qos=1)

    # Accept orders and horizon updates.

    def _handle_order(self, order: dict):
        nodes = sorted(order.get("nodes", []), key=lambda n: n.get("sequenceId", 0))
        edges = sorted(order.get("edges", []), key=lambda e: e.get("sequenceId", 0))
        if not nodes:
            return
        order_id = order.get("orderId", "")
        update_id = order.get("orderUpdateId", 0)

        with self.cond:
            is_update = bool(order_id) and order_id == self.order_id
            if is_update:
                if update_id <= self.order_update_id:
                    return  # Ignore stale order updates
                # Extend the active order and wake the drive loop.
                self.nodes = nodes
                self.edges = edges
                self.order_update_id = update_id
                self.cond.notify_all()

        if is_update:
            released = sum(1 for n in nodes if n.get("released"))
            print(f"[mock] order {order_id[:8]} update {update_id}: "
                  f"released {released}/{len(nodes)}", flush=True)
            return

        print(f"[mock] order {order_id[:8]} -> {nodes[-1].get('nodeId')} "
              f"({len(nodes)} node(s))", flush=True)

        # Stop the previous drive loop before starting a new order.
        self._cancel.set()
        if self._drive_thread and self._drive_thread.is_alive():
            self._drive_thread.join(timeout=2.0)
        self._cancel.clear()

        with self.cond:
            self.order_id = order_id
            self.order_update_id = update_id
            self.nodes = nodes
            self.edges = edges
            # Reset sequence progress for each new order.
            self.last_node_sequence_id = 0

        self._drive_thread = threading.Thread(target=self._drive_route, daemon=True)
        self._drive_thread.start()

    @staticmethod
    def _node_state(node: dict) -> dict:
        return {"nodeId": node.get("nodeId"),
                "sequenceId": node.get("sequenceId", 0),
                "released": bool(node.get("released", True))}

    @staticmethod
    def _edge_state(edge: dict) -> dict:
        return {"edgeId": edge.get("edgeId"),
                "sequenceId": edge.get("sequenceId", 0),
                "released": bool(edge.get("released", True))}

    def _await_release(self, index: int) -> bool:
        """Wait for a route point to be released, or return False if cancelled."""
        while True:
            with self.cond:
                if self._cancel.is_set():
                    self.new_base_request = False
                    return False
                if self.nodes[index].get("released", True):
                    self.new_base_request = False
                    return True
                self.driving = False
                self.new_base_request = True
                self.node_states = [self._node_state(n) for n in self.nodes[index:]]
                self.edge_states = [self._edge_state(e) for e in self.edges[index - 1:]]
            self.publish_state()
            with self.cond:
                if not self.nodes[index].get("released", True) and not self._cancel.is_set():
                    self.cond.wait(timeout=1.0)

    def _drive_route(self):
        """Drive released route points in sequence and publish progress."""
        with self.lock:
            total = len(self.nodes)

        if total < 2:
            with self.lock:
                if self.nodes:
                    self.last_node_id = self.nodes[0].get("nodeId")
                    self.last_node_sequence_id = self.nodes[0].get("sequenceId", 0)
                self.driving = False
                self.node_states = []
                self.edge_states = []
            self.publish_state()
            print(f"[mock] already at {self.last_node_id}", flush=True)
            return

        for index in range(1, total):
            if not self._await_release(index):
                return

            with self.lock:
                node = self.nodes[index]
                pos = node.get("nodePosition", {})
                tx = pos.get("x", self.x)
                ty = pos.get("y", self.y)
                tth = pos.get("theta", self.theta)
                node_id = node.get("nodeId")
                # Report the route points still ahead of the robot.
                self.driving = True
                self.node_states = [self._node_state(n) for n in self.nodes[index:]]
                self.edge_states = [self._edge_state(e) for e in self.edges[index - 1:]]
            self.publish_state()

            sx, sy, sth = self.x, self.y, self.theta
            dist = ((tx - sx) ** 2 + (ty - sy) ** 2) ** 0.5
            steps = max(1, int(dist / 0.3))    # Approximately 0.3 m per step

            for i in range(1, steps + 1):
                if self._cancel.is_set():
                    with self.lock:
                        self.driving = False
                    self.publish_state()
                    return
                f = i / steps
                with self.lock:
                    self.x = sx + (tx - sx) * f
                    self.y = sy + (ty - sy) * f
                    self.theta = sth + (tth - sth) * f
                self.publish_state()
                time.sleep(self.args.step_time)

                # Wait while paused without clearing route progress.
                while self.paused.is_set() and not self._cancel.is_set():
                    with self.lock:
                        self.driving = False
                    self.publish_state()
                    time.sleep(0.5)
                if self._cancel.is_set():
                    with self.lock:
                        self.driving = False
                    self.publish_state()
                    return
                with self.lock:
                    self.driving = True

            last = index + 1 >= total
            with self.lock:
                self.x, self.y, self.theta = tx, ty, tth
                self.last_node_id = node_id
                self.last_node_sequence_id = node.get("sequenceId", 0)
                self.node_states = [self._node_state(n) for n in self.nodes[index + 1:]]
                self.edge_states = [self._edge_state(e) for e in self.edges[index:]]
                self.driving = not last
            self.publish_state()
            print(f"[mock] {'arrived' if last else 'reached'} {node_id}", flush=True)

    def _handle_instant_actions(self, ia: dict):
        for a in ia.get("actions", []):
            kind = a.get("actionType")
            aid = a.get("actionId", "")
            if kind == "stateRequest":
                self.publish_state()
            elif kind == "startPause":
                self.paused.set()
                with self.lock:
                    self.driving = False
                self.publish_state()
                print("[mock] startPause -- holding", flush=True)
            elif kind == "stopPause":
                self.paused.clear()
                self.publish_state()
                print("[mock] stopPause -- carrying on", flush=True)
            elif kind == "cancelOrder":
                self._cancel.set()
                with self.lock:
                    self.driving = False
                    self.new_base_request = False
                    self.node_states = []
                    self.edge_states = []
                    self.order_id = ""
                    self.action_states.append(
                        {"actionId": aid, "actionType": kind,
                         "actionStatus": "FINISHED"})
                self.publish_state()
                print("[mock] cancelOrder", flush=True)

    # Run the mock robot.

    def run(self):
        self.client.connect(self.args.host, self.args.port, keepalive=30)
        self.client.loop_start()
        try:
            while True:
                time.sleep(self.args.state_period)
                self.publish_state()
        except KeyboardInterrupt:
            pass
        finally:
            self.publish_connection("OFFLINE")
            time.sleep(0.2)
            self.client.loop_stop()
            self.client.disconnect()


def main():
    p = argparse.ArgumentParser(description="Simulate a VDA5050 AGV over MQTT.")
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--interface", default="TB3")
    p.add_argument("--manufacturer", default="ROBOTIS")
    p.add_argument("--serial", default="0001")
    p.add_argument("--map", default="tb3_world")
    p.add_argument("--start-node", default="wp1_charging")
    p.add_argument("--x", type=float, default=15.28)
    p.add_argument("--y", type=float, default=-8.80)
    p.add_argument("--theta", type=float, default=0.9)
    p.add_argument("--step-time", type=float, default=0.2,
                   help="seconds per simulated motion step")
    p.add_argument("--state-period", type=float, default=1.0,
                   help="seconds between idle state publishes")
    MockRobot(p.parse_args()).run()


if __name__ == "__main__":
    main()
