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
        client.subscribe(f"{self.base}/mock_control", qos=1)
        self.publish_connection("ONLINE")
        self.publish_factsheet()
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
        elif msg.topic.endswith("/mock_control"):
            self._handle_control(payload)

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

    def publish_factsheet(self):
        """Publish the retained factsheet unless the mock runs without one."""
        if self.args.no_factsheet:
            return
        a = self.args
        actions = [{"actionType": t, "actionScopes": ["INSTANT"], "blockingTypes": ["NONE", "SOFT", "HARD"]}
                   for t in ("startPause", "stopPause", "cancelOrder", "stateRequest", "factsheetRequest")]
        msg = {
            "headerId": self._next_header(),
            "timestamp": iso_now(),
            "version": "2.1.0",
            "manufacturer": a.manufacturer,
            "serialNumber": a.serial,
            "typeSpecification": {
                "seriesName": a.series, "agvKinematic": a.kinematic, "agvClass": a.agv_class,
                "maxLoadMass": 0, "localizationTypes": ["NATURAL"], "navigationTypes": ["AUTONOMOUS"],
            },
            "physicalParameters": {
                "speedMin": 0.0, "speedMax": a.speed_max, "accelerationMax": a.accel_max,
                "decelerationMax": a.accel_max, "heightMax": 0.3, "width": a.width, "length": a.length,
            },
            "protocolLimits": {"maxStringLens": {}, "maxArrayLens": {}, "timing": {
                "minOrderInterval": 0.1, "minStateInterval": 0.1, "defaultStateInterval": 1.0}},
            "protocolFeatures": {"optionalParameters": [], "agvActions": actions},
            "agvGeometry": {}, "loadSpecification": {}, "localizationParameters": {},
        }
        self.client.publish(f"{self.base}/factsheet", json.dumps(msg), qos=1, retain=True)

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
                "batteryState": {"batteryCharge": self.args.battery, "charging": False},
                "agvPosition": {
                    "x": self.x, "y": self.y, "theta": self.theta,
                    "mapId": self.map_id, "positionInitialized": not self.args.pose_uninitialized,
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
                # An update carries the route from its stitching node on: merge it by sequenceId.
                self.nodes = self._merge(self.nodes, nodes, "sequenceId")
                self.edges = self._merge(self.edges, edges, "sequenceId")
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
    def _merge(current: list, update: list, key: str) -> list:
        """Overlay `update` on `current` by `key`, keeping the sequence order."""
        merged = {item.get(key, 0): item for item in current}
        merged.update({item.get(key, 0): item for item in update})
        return [merged[k] for k in sorted(merged)]

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

        index = 0
        while True:
            index += 1
            with self.lock:
                total = len(self.nodes)
            if index >= total:
                return
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

            with self.lock:
                last = index + 1 >= len(self.nodes)
                self.x, self.y, self.theta = tx, ty, tth
                self.last_node_id = node_id
                self.last_node_sequence_id = node.get("sequenceId", 0)
                self.node_states = [self._node_state(n) for n in self.nodes[index + 1:]]
                self.edge_states = [self._edge_state(e) for e in self.edges[index:]]
                self.driving = not last
            self.publish_state()
            print(f"[mock] {'arrived' if last else 'reached'} {node_id}", flush=True)

    def _handle_control(self, control: dict):
        """Test hook on <base>/mock_control: {"pose_initialized": false} makes the robot lose its localization."""
        if isinstance(control.get("pose_initialized"), bool):
            self.args.pose_uninitialized = not control["pose_initialized"]
            print(f"[mock] pose_initialized -> {control['pose_initialized']}", flush=True)
            self.publish_state()

    def _handle_init_position(self, action: dict):
        """Take the pose of an initPosition action and report the robot as localized."""
        raw = action.get("actionParameters") or []
        params = ({p.get("key"): p.get("value") for p in raw if isinstance(p, dict)}
                  if isinstance(raw, list) else dict(raw))
        try:
            x, y, theta = float(params["x"]), float(params["y"]), float(params["theta"])
        except (KeyError, TypeError, ValueError):
            status = "FAILED"
        else:
            status = "FINISHED"
            with self.lock:
                self.x, self.y, self.theta = x, y, theta
                self.map_id = str(params.get("mapId") or self.map_id)
            self.args.pose_uninitialized = False
        with self.lock:
            self.action_states.append({"actionId": action.get("actionId", ""), "actionType": "initPosition",
                                       "actionStatus": status})
        self.publish_state()
        print(f"[mock] initPosition -- {status}", flush=True)

    def _handle_instant_actions(self, ia: dict):
        for a in ia.get("actions", []):
            kind = a.get("actionType")
            aid = a.get("actionId", "")
            if kind == "stateRequest":
                self.publish_state()
            elif kind == "initPosition":
                self._handle_init_position(a)
            elif kind == "factsheetRequest":
                self.publish_factsheet()
                print("[mock] factsheetRequest -- factsheet sent", flush=True)
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
    p.add_argument("--battery", type=float, default=95.0, help="reported battery charge, percent")
    p.add_argument("--pose-uninitialized", action="store_true",
                   help="report positionInitialized=false")
    p.add_argument("--no-factsheet", action="store_true",
                   help="publish no factsheet and ignore factsheetRequest")
    p.add_argument("--series", default="TurtleBot3 Burger", help="factsheet typeSpecification.seriesName")
    p.add_argument("--kinematic", default="DIFF", help="factsheet typeSpecification.agvKinematic")
    p.add_argument("--agv-class", default="CARRIER", help="factsheet typeSpecification.agvClass")
    p.add_argument("--speed-max", type=float, default=0.22, help="factsheet speedMax, m/s")
    p.add_argument("--accel-max", type=float, default=1.0, help="factsheet accelerationMax, m/s^2")
    p.add_argument("--length", type=float, default=0.138, help="factsheet length, m")
    p.add_argument("--width", type=float, default=0.178, help="factsheet width, m")
    MockRobot(p.parse_args()).run()


if __name__ == "__main__":
    main()
