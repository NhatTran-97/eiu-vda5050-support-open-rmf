#!/usr/bin/env python3
"""Publishes state, visualization and connection messages for a range of simulated AGVs (LOAD/<serial>)."""
import argparse
import datetime
import heapq
import random
import time

import paho.mqtt.client as mqtt

NODE = ('{"nodeId":"n%d","sequenceId":%d,"released":true,'
        '"nodePosition":{"x":1.0,"y":0.5,"theta":0.0,"mapId":"map"}}')


def make_state_template(nodes):
    node_list = ",".join(NODE % (i, i * 2) for i in range(nodes))
    return ('{"headerId":%d,"timestamp":"%s","orderId":"","lastNodeId":"n0","driving":false,'
            '"nodeStates":[' + node_list + '],"edgeStates":[],"actionStates":[],"errors":[],'
            '"operatingMode":"AUTOMATIC","safetyState":{"eStop":"NONE","fieldViolation":false},'
            '"batteryState":{"batteryCharge":80.0},'
            '"agvPosition":{"x":1.0,"y":2.0,"theta":0.0,"mapId":"map","positionInitialized":true}}')


VIZ = ('{"headerId":%d,"timestamp":"%s","agvPosition":{"x":1.0,"y":2.0,"theta":0.0,"mapId":"map",'
       '"positionInitialized":true},"velocity":{"vx":0.2,"vy":0.0,"omega":0.0}}')


def stamp():
    now = datetime.datetime.now(datetime.timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (now.microsecond // 1000)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=18830)
    p.add_argument("--first", type=int, required=True)
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--state-hz", type=float, default=2.0)
    p.add_argument("--viz-hz", type=float, default=4.0)
    p.add_argument("--nodes", type=int, default=20)
    p.add_argument("--seconds", type=float, default=90.0)
    args = p.parse_args()

    client = mqtt.Client(client_id="load_gen_%d" % args.first)
    client.connect(args.host, args.port)
    client.loop_start()

    template = make_state_template(args.nodes)
    robots = range(args.first, args.first + args.count)
    for i in robots:
        client.publish("AMR/v2/LOAD/%04d/connection" % i, '{"connectionState":"ONLINE"}', qos=1, retain=True)

    header = {i: 1 for i in robots}
    queue = []
    start = time.monotonic()
    for i in robots:
        if args.state_hz > 0:
            heapq.heappush(queue, (start + random.random() / args.state_hz, i, "state"))
        if args.viz_hz > 0:
            heapq.heappush(queue, (start + random.random() / args.viz_hz, i, "visualization"))
    period = {"state": 1.0 / args.state_hz if args.state_hz > 0 else 0, "visualization": 1.0 / args.viz_hz if args.viz_hz > 0 else 0}
    late = 0
    sent = 0
    end = start + args.seconds
    while queue and time.monotonic() < end:
        due, i, kind = heapq.heappop(queue)
        wait = due - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        elif wait < -0.05:
            late += 1
        payload = (template if kind == "state" else VIZ) % (header[i], stamp())
        header[i] += 1
        client.publish("AMR/v2/LOAD/%04d/%s" % (i, kind), payload, qos=0)
        sent += 1
        heapq.heappush(queue, (due + period[kind], i, kind))
    client.loop_stop()
    client.disconnect()
    print("GEN first=%d count=%d sent=%d late_over_50ms=%d" % (args.first, args.count, sent, late), flush=True)


if __name__ == "__main__":
    main()
