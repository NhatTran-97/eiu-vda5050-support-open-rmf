#!/usr/bin/env python3
"""
mock_mqtt_robot.py — simulate a VDA5050 AGV over MQTT.

Replaces the entire robot side (vda5050_client_adapter + tb3_vda5050_bridge +
Nav2) so the vda5050_fleet_adapter can be exercised end-to-end without hardware.

Behaviour:
  * publishes `connection` ONLINE on start (retained),
  * answers a `stateRequest` instant action with an immediate `state`,
  * on each `order`, simulates driving from the current pose to the end node,
    streaming `state` (driving=true) and finally reporting arrival
    (lastNodeId = end node, driving=false, node/edge states cleared),
  * on `cancelOrder`, stops and clears the active order,
  * publishes periodic `state`.

The identity flags MUST match the adapter's `vda5050:` config block.

Usage:
  python3 mock_mqtt_robot.py
  python3 mock_mqtt_robot.py --host localhost --interface TB3 \
      --manufacturer ROBOTIS --serial 0001 \
      --x 15.28 --y -8.80 --theta 0.9 --start-node wp1_charging
"""
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
        self.node_states = []
        self.edge_states = []
        self.action_states = []
        self.header = 0

        self.lock = threading.Lock()
        self._drive_thread = None
        self._cancel = threading.Event()
        # Set by startPause, cleared by stopPause. The drive loop waits on it
        # without dropping the order, the way a VDA5050 client holds.
        self.paused = threading.Event()

        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        # Last-will so the broker marks us OFFLINE if we crash.
        self.client.will_set(
            f"{self.base}/connection",
            json.dumps(self._connection_payload("CONNECTIONBROKEN")),
            qos=1, retain=True)

    # ── MQTT plumbing ──────────────────────────────────────────────────────────

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

    # ── Publishers ─────────────────────────────────────────────────────────────

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

    # ── Order handling ─────────────────────────────────────────────────────────

    def _handle_order(self, order: dict):
        nodes = sorted(order.get("nodes", []), key=lambda n: n.get("sequenceId", 0))
        edges = sorted(order.get("edges", []), key=lambda e: e.get("sequenceId", 0))
        if not nodes:
            return
        order_id = order.get("orderId", "")
        print(f"[mock] order {order_id[:8]} -> {nodes[-1].get('nodeId')} "
              f"({len(nodes)} node(s))", flush=True)

        # Interrupt any drive already in flight.
        self._cancel.set()
        if self._drive_thread and self._drive_thread.is_alive():
            self._drive_thread.join(timeout=2.0)
        self._cancel.clear()

        with self.lock:
            self.order_id = order_id
            self.order_update_id = order.get("orderUpdateId", 0)

        self._drive_thread = threading.Thread(
            target=self._drive_route, args=(nodes, edges), daemon=True)
        self._drive_thread.start()

    @staticmethod
    def _node_state(node: dict) -> dict:
        return {"nodeId": node.get("nodeId"),
                "sequenceId": node.get("sequenceId", 0),
                "released": True}

    @staticmethod
    def _edge_state(edge: dict) -> dict:
        return {"edgeId": edge.get("edgeId"),
                "sequenceId": edge.get("sequenceId", 0),
                "released": True}

    def _drive_route(self, nodes: list, edges: list):
        """Execute a whole order the way a VDA5050 client does: drive the nodes
        in sequenceId order, reporting lastNodeId at each one and draining
        nodeStates/edgeStates as they are passed. nodes[0] is the base node the
        AGV is already standing on."""
        remaining = nodes[1:]
        if not remaining:
            node_id = nodes[0].get("nodeId")
            with self.lock:
                self.last_node_id = node_id
                self.last_node_sequence_id = nodes[0].get("sequenceId", 0)
                self.driving = False
                self.node_states = []
                self.edge_states = []
            self.publish_state()
            print(f"[mock] already at {node_id}", flush=True)
            return

        for index, node in enumerate(remaining):
            pos = node.get("nodePosition", {})
            tx = pos.get("x", self.x)
            ty = pos.get("y", self.y)
            tth = pos.get("theta", self.theta)
            node_id = node.get("nodeId")

            # Everything still ahead of the AGV, including the node it is
            # driving towards and the edge it is on.
            with self.lock:
                self.driving = True
                self.node_states = [self._node_state(n) for n in remaining[index:]]
                self.edge_states = [self._edge_state(e) for e in edges[index:]]
            self.publish_state()

            sx, sy, sth = self.x, self.y, self.theta
            dist = ((tx - sx) ** 2 + (ty - sy) ** 2) ** 0.5
            steps = max(1, int(dist / 0.3))    # ~0.3 m per step

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

                # Hold here for as long as startPause is in force. The order,
                # the route and the progress so far all stay put.
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

            last = index + 1 >= len(remaining)
            with self.lock:
                self.x, self.y, self.theta = tx, ty, tth
                self.last_node_id = node_id
                self.last_node_sequence_id = node.get("sequenceId", 0)
                self.node_states = [self._node_state(n) for n in remaining[index + 1:]]
                self.edge_states = [self._edge_state(e) for e in edges[index + 1:]]
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
                    self.node_states = []
                    self.edge_states = []
                    self.order_id = ""
                    self.action_states.append(
                        {"actionId": aid, "actionType": kind,
                         "actionStatus": "FINISHED"})
                self.publish_state()
                print("[mock] cancelOrder", flush=True)

    # ── Main loop ──────────────────────────────────────────────────────────────

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
