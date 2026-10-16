#!/usr/bin/env python3
"""Start a broker from setup_broker.py output and check its TLS, accounts and ACL."""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import paho.mqtt.client as mqtt

HERE = os.path.dirname(os.path.abspath(__file__))
FLEET_CONFIG = '''vda5050:
  interface_name: "AMR"
  robots:
    r1: {manufacturer: "ROBOTIS", serial: "0001"}
    r2: {manufacturer: "ROBOTIS", serial: "0002"}
'''


def free_port():
    """A local TCP port nobody listens on."""
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Peer:
    """One MQTT connection that records the messages it receives."""

    def __init__(self, port, ca, user=None, password=None, tls=True):
        self.messages = []
        self.refused = None
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f'check_{user}_{time.monotonic_ns()}')
        if tls:
            self.client.tls_set(ca_certs=ca)
        if user:
            self.client.username_pw_set(user, password)
        self.client.on_message = lambda c, u, m: self.messages.append((m.topic, m.payload.decode()))
        self.client.on_connect = self._on_connect
        self.connected = False
        try:
            self.client.connect('127.0.0.1', port)
        except OSError as e:
            self.refused = str(e)
            return
        self.client.loop_start()
        deadline = time.time() + 5
        while not self.connected and self.refused is None and time.time() < deadline:
            time.sleep(0.05)

    def _on_connect(self, client, userdata, flags, reason, properties):
        if reason.is_failure:
            self.refused = str(reason)
        else:
            self.connected = True

    def subscribe(self, topic):
        self.client.subscribe(topic, qos=1)
        time.sleep(0.3)

    def publish(self, topic, payload='{}'):
        self.client.publish(topic, payload, qos=1).wait_for_publish(3)

    def close(self):
        self.client.loop_stop()
        self.client.disconnect()


def test_acl_and_tls():
    mosquitto = os.environ.get('MOSQUITTO') or shutil.which('mosquitto') or '/usr/sbin/mosquitto'
    if not os.path.exists(mosquitto):
        print('SKIP: no mosquitto binary')
        return
    with tempfile.TemporaryDirectory() as tmp:
        config = os.path.join(tmp, 'fleet.yaml')
        with open(config, 'w') as f:
            f.write(FLEET_CONFIG)
        out = os.path.join(tmp, 'broker')
        port = free_port()
        subprocess.run([sys.executable, os.path.join(HERE, 'setup_broker.py'), '--out', out, '--config', config,
                        '--agv', 'AMR/EIU/0009', '--host', 'localhost', '127.0.0.1', '--port', str(port)], check=True)
        with open(os.path.join(out, 'credentials.txt')) as f:
            creds = dict(line.strip().split(':', 1) for line in f)
        assert set(creds) == {'fleet_master', 'fleet_ui', 'ROBOTIS_0001', 'ROBOTIS_0002', 'EIU_0009'}, creds

        broker = subprocess.Popen([mosquitto, '-c', os.path.join(out, 'mosquitto.conf')],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ca = os.path.join(out, 'certs', 'ca.crt')
        peers = []
        try:
            time.sleep(0.5)

            def peer(user=None, password=None, tls=True):
                p = Peer(port, ca, user, creds.get(user) if password is None else password, tls)
                peers.append(p)
                return p

            assert not peer('fleet_master', 'wrong').connected, 'a wrong password is refused'
            assert not peer().connected, 'an anonymous client is refused'
            assert not peer('fleet_master', tls=False).connected, 'plain TCP is refused'

            master, ui = peer('fleet_master'), peer('fleet_ui')
            agv1, agv2 = peer('ROBOTIS_0001'), peer('ROBOTIS_0002')
            assert all(p.connected for p in (master, ui, agv1, agv2)), [p.refused for p in (master, ui, agv1, agv2)]

            agv1.subscribe('AMR/v2/ROBOTIS/0001/order')
            agv1.subscribe('AMR/v2/ROBOTIS/0002/order')
            master.subscribe('AMR/v2/+/+/state')
            ui.subscribe('AMR/v2/+/+/state')

            master.publish('AMR/v2/ROBOTIS/0001/order', 'from master')
            ui.publish('AMR/v2/ROBOTIS/0001/order', 'from ui')
            agv2.publish('AMR/v2/ROBOTIS/0001/order', 'from agv2')
            master.publish('AMR/v2/ROBOTIS/0002/order', 'to agv2')
            agv1.publish('AMR/v2/ROBOTIS/0001/state', 'agv1 state')
            agv2.publish('AMR/v2/ROBOTIS/0001/state', 'agv2 posing as agv1')
            master.publish('AMR/v2/ROBOTIS/0001/state', 'master posing as agv1')
            time.sleep(1.0)

            assert agv1.messages == [('AMR/v2/ROBOTIS/0001/order', 'from master')], agv1.messages
            assert master.messages == [('AMR/v2/ROBOTIS/0001/state', 'agv1 state')], master.messages
            assert ui.messages == [('AMR/v2/ROBOTIS/0001/state', 'agv1 state')], ui.messages
        finally:
            for p in peers:
                p.close()
            broker.terminate()
            broker.wait(5)
    print('broker TLS, accounts and ACL: OK')


if __name__ == '__main__':
    test_acl_and_tls()
