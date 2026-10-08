"""Link to Open-RMF through the eiu_rmf_gateway over Redis (contract: eiu_rmf_gateway/docs/contract.md).

The backend has no ROS 2 node: it writes commands to the gateway's command stream and reads its
event stream, fleet states, workcell states and heartbeat.
"""

import json
import logging
import time

import redis

log = logging.getLogger("eiu_web.rmf")

VERSION = 1
COMMANDS_MAXLEN = 1000
EVENTS_PER_READ = 500

OPS_HASHES = ("adapters", "controls", "metrics", "lanes", "registry", "discovery")

MODE_ACTIVITY = {0: "idle", 1: "idle", 2: "moving", 3: "waiting", 4: "waiting", 5: "waiting",
                 6: "moving", 7: "moving", 8: "waiting", 9: "moving"}
# rmf_fleet_msgs/RobotMode.mode
MODE_NAMES = {0: "idle", 1: "charging", 2: "moving", 3: "paused", 4: "waiting", 5: "emergency",
              6: "going_home", 7: "docking", 8: "adapter_error", 9: "cleaning"}


def now_ms() -> int:
    return int(time.time() * 1000)


class GatewayClient:
    def __init__(self, url: str, prefix: str, cursor_key: str, client=None):
        self._redis = client or redis.Redis.from_url(url, decode_responses=True, socket_timeout=2,
                                                     socket_connect_timeout=2)
        self._commands = f"{prefix}:commands"
        self._events = f"{prefix}:events"
        self._fleets = f"{prefix}:fleets"
        self._workcells = f"{prefix}:workcells"
        self._heartbeat = f"{prefix}:gateway"
        self._ops_hashes = {name: f"{prefix}:{name}" for name in OPS_HASHES}
        self._nav_graph = f"{prefix}:nav_graph"
        self._cursor_key = cursor_key
        self._cursor: str | None = None
        self.available = False
        self.gateway: dict = {}
        # Last /fleet_states time of each fleet (epoch ms).
        self.fleets_received: dict[str, int] = {}
        # Latest operations state from the gateway: hash name -> field -> object, plus "nav_graph".
        self.ops: dict = {name: {} for name in OPS_HASHES}
        self.ops["nav_graph"] = None

    # Commands

    def send(self, request_id: str, envelope: dict) -> bool:
        """Queue an Open-RMF task API request for the gateway; False when Redis cannot be reached."""
        return self.command("task_request", request_id, envelope)

    def command(self, cmd_type: str, cmd_id: str, body: dict) -> bool:
        """Queue any gateway command; False when Redis cannot be reached."""
        msg = json.dumps({"v": VERSION, "id": cmd_id, "type": cmd_type, "sent_ms": now_ms(), "body": body},
                         separators=(",", ":"))
        try:
            self._redis.xadd(self._commands, {"msg": msg}, maxlen=COMMANDS_MAXLEN, approximate=True)
            return True
        except redis.RedisError as e:
            log.warning("command %s (%s) not queued: %s", cmd_id, cmd_type, e)
            self.available = False
            return False

    # Events

    def drain(self) -> list[tuple]:
        """Gateway events since the last call, as tracker events."""
        try:
            if self._cursor is None:
                self._cursor = self._redis.get(self._cursor_key) or self._last_event_id()
            replies = self._redis.xread({self._events: self._cursor}, count=EVENTS_PER_READ)
        except redis.RedisError as e:
            log.warning("events not read: %s", e)
            self.available = False
            return []
        out = []
        for _stream, entries in replies or []:
            for entry_id, fields in entries:
                self._cursor = entry_id
                event = self._decode(fields.get("msg"))
                if event is not None:
                    out.append(event)
        if out:
            try:
                self._redis.set(self._cursor_key, self._cursor)
            except redis.RedisError:
                pass
        return out

    def _last_event_id(self) -> str:
        last = self._redis.xrevrange(self._events, count=1)
        return last[0][0] if last else "0-0"

    @staticmethod
    def _decode(raw) -> tuple | None:
        try:
            msg = json.loads(raw)
        except (TypeError, ValueError):
            return None
        if not isinstance(msg, dict) or msg.get("v") != VERSION or not isinstance(msg.get("body"), dict):
            return None
        body = msg["body"]
        kind = msg.get("type")
        if kind == "task_api_response":
            return ("response", body.get("request_id", ""), body.get("response"))
        if kind == "dispatch_states":
            return ("dispatch", body.get("states") or [])
        if kind == "task_state":
            return ("task_state", body.get("state") or {})
        if kind == "command_result":
            return ("command_result", body.get("id", ""), bool(body.get("ok")), body.get("error", ""),
                    body.get("message", ""))
        if kind == "registration_result":
            return ("registration_result", body.get("id", ""), body.get("result") or {})
        return None

    # State

    def snapshot(self, offline_s: float) -> tuple[dict, bool, dict]:
        """(robots by name, whether any fleet reported within offline_s, workcells by guid)."""
        try:
            pipe = self._redis.pipeline().get(self._heartbeat).hgetall(self._fleets).hgetall(self._workcells)
            for name in OPS_HASHES:
                pipe = pipe.hgetall(self._ops_hashes[name])
            replies = pipe.get(self._nav_graph).execute()
            beat, fleets, cells = replies[:3]
            for name, raw in zip(OPS_HASHES, replies[3:3 + len(OPS_HASHES)]):
                self.ops[name] = {field: json.loads(value) for field, value in (raw or {}).items()}
            self.ops["nav_graph"] = json.loads(replies[-1]) if replies[-1] else None
        except redis.RedisError as e:
            log.warning("state not read: %s", e)
            self.available = False
            return {}, False, {}
        self.gateway = json.loads(beat) if beat else {}
        self.available = bool(beat)
        now = now_ms()
        robots, online = {}, False
        for raw in (fleets or {}).values():
            fleet = json.loads(raw)
            if fleet.get("v") != VERSION:
                continue
            self.fleets_received[fleet["fleet"]] = int(fleet.get("received_ms", 0))
            stale = now - int(fleet.get("received_ms", 0)) > offline_s * 1000
            online |= not stale
            for r in fleet.get("robots", []):
                robots[r["name"]] = {
                    "name": r["name"], "fleet": fleet["fleet"], "level": r.get("level", ""),
                    "x": float(r["x"]), "y": float(r["y"]), "yaw": float(r.get("yaw", 0.0)),
                    "battery": float(r.get("battery", 0.0)), "activity": MODE_ACTIVITY.get(r.get("mode"), "idle"),
                    "mode": MODE_NAMES.get(r.get("mode"), "idle"),
                    "task_id": r.get("task_id", ""), "path": [tuple(p) for p in r.get("path", [])],
                    "path_end_ms": r.get("path_end_ms"), "stale": stale,
                    "telemetry": r.get("telemetry") if isinstance(r.get("telemetry"), dict) else {},
                }
        workcells = {}
        for guid, raw in (cells or {}).items():
            cell = json.loads(raw)
            workcells[guid] = {"kind": cell.get("kind"), "busy": bool(cell.get("busy")),
                               "seconds": float(cell.get("seconds_remaining", 0.0))}
        return robots, online and self.available, workcells
