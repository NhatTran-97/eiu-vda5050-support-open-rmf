import json
import time

from eiu_web_backend.rmf.client import GatewayClient


class FakeRedis:
    def __init__(self):
        self.streams = {"p:events": []}
        self.hashes = {}
        self.strings = {}

    def xadd(self, name, fields, maxlen=None, approximate=True):
        entries = self.streams.setdefault(name, [])
        entries.append((f"{len(entries) + 1}-0", dict(fields)))

    def xread(self, streams, count=None):
        (name, cursor), = streams.items()
        n = int(cursor.split("-")[0])
        entries = self.streams.get(name, [])[n:]
        return [(name, entries)] if entries else []

    def xrevrange(self, name, count=1):
        entries = self.streams.get(name, [])
        return entries[-1:] if entries else []

    def get(self, key):
        return self.strings.get(key)

    def set(self, key, value):
        self.strings[key] = value

    def hgetall(self, key):
        return dict(self.hashes.get(key, {}))

    def pipeline(self):
        return Pipe(self)


class Pipe:
    def __init__(self, r):
        self.r, self.calls = r, []

    def get(self, key):
        self.calls.append(("get", key))
        return self

    def hgetall(self, key):
        self.calls.append(("hgetall", key))
        return self

    def execute(self):
        return [getattr(self.r, op)(key) for op, key in self.calls]


def event(kind, body):
    return {"msg": json.dumps({"v": 1, "type": kind, "at_ms": 1, "body": body})}


def make():
    r = FakeRedis()
    return GatewayClient("redis://unused", "p", "cursor", client=r), r


def test_send_writes_a_versioned_task_request():
    client, r = make()
    assert client.send("eiu-web-1", {"type": "cancel_task_request", "task_id": "t"})
    msg = json.loads(r.streams["p:commands"][0][1]["msg"])
    assert msg["v"] == 1 and msg["id"] == "eiu-web-1" and msg["type"] == "task_request"
    assert msg["body"]["task_id"] == "t" and msg["sent_ms"] > 0


def test_drain_starts_at_the_end_and_resumes_from_the_cursor():
    client, r = make()
    r.xadd("p:events", event("dispatch_states", {"states": []}))
    assert client.drain() == []                       # older events are skipped on the first start
    r.xadd("p:events", event("task_api_response", {"request_id": "a", "response": {"success": True}}))
    r.xadd("p:events", event("command_result", {"id": "b", "ok": False, "error": "expired"}))
    r.xadd("p:events", event("command_result", {"id": "c", "ok": True, "error": ""}))
    r.xadd("p:events", event("task_state", {"state": {"booking": {"id": "x"}}}))
    assert client.drain() == [("response", "a", {"success": True}), ("command_result", "b", False, "expired", ""),
                              ("command_result", "c", True, "", ""), ("task_state", {"booking": {"id": "x"}})]
    assert r.strings["cursor"] == "5-0"
    again = GatewayClient("redis://unused", "p", "cursor", client=r)
    r.xadd("p:events", event("dispatch_states", {"states": [{"task_id": "x"}]}))
    assert again.drain() == [("dispatch", [{"task_id": "x"}])]


def test_snapshot_reads_fleets_workcells_and_heartbeat():
    client, r = make()
    now = int(time.time() * 1000)
    r.hashes["p:fleets"] = {
        "tb3_fleet": json.dumps({"v": 1, "fleet": "tb3_fleet", "received_ms": now, "robots": [
            {"name": "tb3_1", "level": "L1", "x": 1, "y": 2, "yaw": 0, "battery": 80, "mode": 2,
             "task_id": "t1", "path": [[3, 4]], "path_end_ms": None}]}),
        "old_fleet": json.dumps({"v": 1, "fleet": "old_fleet", "received_ms": now - 60_000, "robots": [
            {"name": "r9", "x": 0, "y": 0, "mode": 0}]}),
    }
    r.hashes["p:workcells"] = {"mock_dispenser_1": json.dumps({"v": 1, "kind": "dispenser", "busy": True,
                                                              "seconds_remaining": 2.0})}
    robots, online, cells = client.snapshot(5.0)
    assert online is False and client.available is False   # no heartbeat
    r.strings["p:gateway"] = json.dumps({"v": 1, "ros_domain_id": "7"})
    robots, online, cells = client.snapshot(5.0)
    assert online and client.available and client.gateway["ros_domain_id"] == "7"
    assert robots["tb3_1"]["activity"] == "moving" and robots["tb3_1"]["path"] == [(3, 4)] and not robots["tb3_1"]["stale"]
    assert robots["r9"]["stale"]
    assert cells["mock_dispenser_1"]["busy"] is True
