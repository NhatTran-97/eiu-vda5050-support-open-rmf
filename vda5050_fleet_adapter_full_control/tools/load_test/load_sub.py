#!/usr/bin/env python3
"""Control subscriber: how old the simulated AGVs' state messages are when a plain Python client receives them."""
import argparse
import datetime
import time

import paho.mqtt.client as mqtt

delays = []


def on_message(client, userdata, msg):
    payload = msg.payload
    i = payload.find(b'"timestamp":"') + 13
    text = payload[i:i + 24].decode()
    sent = datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=datetime.timezone.utc)
    delays.append((datetime.datetime.now(datetime.timezone.utc) - sent).total_seconds() * 1000.0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=18830)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--skip", type=float, default=15)
    a = p.parse_args()
    client = mqtt.Client(client_id="load_sub")
    client.on_message = on_message
    client.connect("127.0.0.1", a.port)
    client.subscribe("AMR/v2/LOAD/+/state", qos=0)
    client.loop_start()
    time.sleep(a.skip)
    del delays[:]
    time.sleep(a.seconds)
    client.loop_stop()
    d = sorted(delays)
    if d:
        print("SUB count=%d p50=%.1f p99=%.1f max=%.1f ms" % (len(d), d[len(d) // 2], d[int(len(d) * 0.99)], d[-1]), flush=True)


if __name__ == "__main__":
    main()
