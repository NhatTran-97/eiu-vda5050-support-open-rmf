"""Gateway logic between the ROS side and Redis; plain Python, so it is tested without ROS."""

import json
import logging
import threading

from . import contract

log = logging.getLogger('eiu_rmf_gateway')

# Hash categories written to Redis: Keys attribute -> field -> JSON object.
HASHES = ('fleets', 'workcells', 'adapters', 'controls', 'metrics', 'lanes', 'registry', 'discovery')
# Single-value keys.
VALUES = ('nav_graph',)


class Recorder:
    """Data from ROS callbacks and the task events socket, kept until the next flush.

    Callbacks run on other threads; they only store data.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._events: list[str] = []
        self._hashes: dict[str, dict[str, dict]] = {name: {} for name in HASHES}
        self._dirty: dict[str, set[str]] = {name: set() for name in HASHES}
        self._values: dict[str, dict] = {}
        self._dirty_values: set[str] = set()

    def add_event(self, event_type: str, body: dict) -> None:
        with self._lock:
            self._events.append(contract.event(event_type, body))

    def command_result(self, command_id: str, ok: bool, error: str = '', message: str = '', **extra) -> None:
        self.add_event('command_result', {'id': command_id, 'ok': ok, 'error': error, 'message': message, **extra})

    def put(self, category: str, field: str, obj: dict, always: bool = False) -> None:
        """Store the latest object of `field`; it is written at the next flush when it changed (or `always`)."""
        obj = {'v': contract.VERSION, **obj}
        with self._lock:
            previous = self._hashes[category].get(field)
            self._hashes[category][field] = obj
            if always or previous is None or _without_time(previous) != _without_time(obj):
                self._dirty[category].add(field)

    def get(self, category: str, field: str) -> dict | None:
        with self._lock:
            obj = self._hashes[category].get(field)
            return dict(obj) if obj else None

    def fields(self, category: str) -> list[str]:
        with self._lock:
            return list(self._hashes[category])

    def set_value(self, name: str, obj: dict) -> None:
        with self._lock:
            self._values[name] = {'v': contract.VERSION, **obj}
            self._dirty_values.add(name)

    def set_fleet(self, fleet: str, robots: list[dict]) -> None:
        self.put('fleets', fleet, {'fleet': fleet, 'received_ms': contract.now_ms(), 'robots': robots}, always=True)

    def set_workcell(self, guid: str, kind: str, busy: bool, seconds_remaining: float) -> None:
        self.put('workcells', guid, {'guid': guid, 'kind': kind, 'busy': busy,
                                     'seconds_remaining': seconds_remaining, 'received_ms': contract.now_ms()})

    def take(self) -> tuple[list[str], dict[str, dict[str, str]], dict[str, str]]:
        """Events, the changed hash fields per category, and the changed values, as encoded JSON."""
        with self._lock:
            events, self._events = self._events, []
            hashes = {}
            for category, fields in self._dirty.items():
                if fields:
                    hashes[category] = {f: contract.encode(self._hashes[category][f]) for f in fields}
                    fields.clear()
            values = {name: contract.encode(self._values[name]) for name in self._dirty_values}
            self._dirty_values.clear()
        return events, hashes, values


def _without_time(obj: dict) -> dict:
    return {k: v for k, v in obj.items() if k not in ('received_ms', 'at_ms')}


class Gateway:
    """Reads commands from Redis, hands them to `execute`, and writes what the Recorder holds.

    `execute(command_type, command_id, body)` returns (ok, error, message) when the result is known at once,
    or None when the ROS side reports it later through `Recorder.command_result`.
    """

    def __init__(self, redis, keys: contract.Keys, recorder: Recorder, execute, *, events_maxlen: int,
                 command_max_age_s: float, heartbeat_ttl_s: float, consumer: str = 'gateway-1',
                 info: dict | None = None):
        self.redis = redis
        self.keys = keys
        self.recorder = recorder
        self.execute = execute
        self.events_maxlen = events_maxlen
        self.max_age_ms = int(command_max_age_s * 1000)
        self.heartbeat_ttl_ms = int(heartbeat_ttl_s * 1000)
        self.consumer = consumer
        self.info = info or {}
        self.started_ms = contract.now_ms()
        self._pending_checked = False

    def ensure_group(self) -> None:
        try:
            self.redis.xgroup_create(self.keys.commands, contract.GROUP, id='0', mkstream=True)
        except Exception as e:  # BUSYGROUP: the group exists
            if 'BUSYGROUP' not in str(e):
                raise

    def handle_commands(self, block_ms: int) -> int:
        """Read and execute new commands (pending ones first after a start); returns how many were handled."""
        stream_id = '0' if not self._pending_checked else '>'
        replies = self.redis.xreadgroup(contract.GROUP, self.consumer, {self.keys.commands: stream_id},
                                        count=50, block=None if stream_id == '0' else block_ms)
        if stream_id == '0':
            self._pending_checked = True
        handled = 0
        for _stream, entries in replies or []:
            for entry_id, fields in entries:
                self._run(fields.get('msg') or fields.get(b'msg'))
                self.redis.xack(self.keys.commands, contract.GROUP, entry_id)
                handled += 1
        return handled

    def _run(self, raw) -> None:
        if isinstance(raw, bytes):
            raw = raw.decode()
        cmd, error = contract.check_command(raw, contract.now_ms(), self.max_age_ms)
        cmd_id = cmd.get('id', '') if isinstance(cmd, dict) else ''
        if error:
            log.warning('command %s refused: %s', cmd_id or '?', error)
            self.recorder.command_result(cmd_id, False, error)
            return
        try:
            result = self.execute(cmd['type'], cmd_id, cmd['body'])
        except Exception as e:
            log.error('command %s (%s) failed: %s', cmd_id, cmd['type'], e)
            self.recorder.command_result(cmd_id, False, 'publish_failed', str(e))
            return
        if result is not None:
            ok, error, message = result
            self.recorder.command_result(cmd_id, ok, error, message)

    def flush(self) -> None:
        """Write the recorded events and changed states, and renew the heartbeat."""
        events, hashes, values = self.recorder.take()
        pipe = self.redis.pipeline()
        for msg in events:
            pipe.xadd(self.keys.events, {'msg': msg}, maxlen=self.events_maxlen, approximate=True)
        for category, fields in hashes.items():
            pipe.hset(getattr(self.keys, category), mapping=fields)
        for name, value in values.items():
            pipe.set(getattr(self.keys, name), value)
        pipe.set(self.keys.gateway, contract.encode({'v': contract.VERSION, 'started_ms': self.started_ms,
                                                     'at_ms': contract.now_ms(), **self.info}),
                 px=self.heartbeat_ttl_ms)
        pipe.execute()


def fleet_robots(msg) -> list[dict]:
    """The robots of an rmf_fleet_msgs/FleetState as contract dicts."""
    robots = []
    for r in msg.robots:
        loc = r.location
        end = r.path[-1].t if r.path else None
        robots.append({
            'name': r.name, 'level': loc.level_name, 'x': round(float(loc.x), 4), 'y': round(float(loc.y), 4),
            'yaw': round(float(loc.yaw), 4), 'battery': round(float(r.battery_percent), 2), 'mode': int(r.mode.mode),
            'task_id': r.task_id or '', 'path': [[round(float(p.x), 4), round(float(p.y), 4)] for p in r.path],
            'path_end_ms': int(end.sec * 1000 + end.nanosec / 1e6) if end and end.sec > 0 else None,
        })
    return robots


def dispatch_states(msg) -> list[dict]:
    states = []
    for s in list(msg.active) + list(msg.finished):
        a = s.assignment
        states.append({'task_id': s.task_id, 'status': int(s.status),
                       'robot': a.expected_robot_name if a.is_assigned else '',
                       'fleet': a.fleet_name if a.is_assigned else '',
                       'errors': [e if isinstance(e, str) else str(e) for e in s.errors]})
    return states


def parse_json(text: str):
    try:
        return json.loads(text)
    except ValueError:
        return {'raw': text}
