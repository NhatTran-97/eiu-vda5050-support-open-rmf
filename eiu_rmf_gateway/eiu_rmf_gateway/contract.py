"""Keys and messages of the gateway contract (docs/contract.md); plain Python, no ROS."""

import json
import math
import time

VERSION = 1
GROUP = 'gateway'
TASK_REQUEST_TYPES = frozenset({'dispatch_task_request', 'robot_task_request', 'cancel_task_request',
                                'interrupt_task_request', 'resume_task_request'})
REGISTRATION_ACTIONS = frozenset({'add', 'remove'})


def now_ms() -> int:
    return int(time.time() * 1000)


class Keys:
    def __init__(self, prefix: str):
        self.commands = f'{prefix}:commands'
        self.events = f'{prefix}:events'
        self.fleets = f'{prefix}:fleets'
        self.workcells = f'{prefix}:workcells'
        self.gateway = f'{prefix}:gateway'
        self.adapters = f'{prefix}:adapters'
        self.controls = f'{prefix}:controls'
        self.metrics = f'{prefix}:metrics'
        self.lanes = f'{prefix}:lanes'
        self.registry = f'{prefix}:registry'
        self.discovery = f'{prefix}:discovery'
        self.nav_graph = f'{prefix}:nav_graph'


def encode(obj: dict) -> str:
    return json.dumps(obj, separators=(',', ':'))


def event(event_type: str, body: dict, at_ms: int | None = None) -> str:
    return encode({'v': VERSION, 'type': event_type, 'at_ms': at_ms or now_ms(), 'body': body})


def _name(value) -> bool:
    return isinstance(value, str) and 0 < len(value) <= 128


def _number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _indices(value) -> bool:
    return isinstance(value, list) and all(isinstance(i, int) and not isinstance(i, bool) and i >= 0 for i in value)


def _task_request(body) -> bool:
    return body.get('type') in TASK_REQUEST_TYPES


def _robot(body) -> bool:
    return _name(body.get('robot'))


def _speed_limit(body) -> bool:
    return _robot(body) and _number(body.get('mps')) and body['mps'] >= 0


def _init_position(body) -> bool:
    return _robot(body) and all(_number(body.get(k)) for k in ('x', 'y', 'yaw'))


def _registration(body) -> bool:
    request = body.get('request')
    return (isinstance(request, dict) and request.get('action') in REGISTRATION_ACTIONS
            and _name(request.get('fleet')) and _name(request.get('name')))


def _lane_request(body) -> bool:
    return (_name(body.get('fleet')) and _indices(body.get('open_lanes', [])) and _indices(body.get('close_lanes', []))
            and bool(body.get('open_lanes') or body.get('close_lanes')))


def _nav_graph_save(body) -> bool:
    return isinstance(body.get('yaml'), str) and isinstance(body.get('base_sha256'), str)


# Command type -> check of its body.
COMMANDS = {
    'task_request': _task_request,
    'robot_pause': _robot,
    'robot_resume': _robot,
    'robot_speed_limit': _speed_limit,
    'robot_init_position': _init_position,
    'registration_request': _registration,
    'lane_request': _lane_request,
    'nav_graph_save': _nav_graph_save,
}


def check_command(raw: str, now: int, max_age_ms: int) -> tuple[dict | None, str]:
    """(command, '') when it may be executed, else (command or None, error code)."""
    try:
        cmd = json.loads(raw)
    except (TypeError, ValueError):
        return None, 'bad_json'
    if not isinstance(cmd, dict) or not isinstance(cmd.get('id'), str) or not cmd['id']:
        return None, 'bad_command'
    if cmd.get('v') != VERSION:
        return cmd, 'bad_version'
    check = COMMANDS.get(cmd.get('type'))
    if check is None:
        return cmd, 'unknown_type'
    body = cmd.get('body')
    if not isinstance(body, dict) or not check(body):
        return cmd, 'bad_body'
    sent = cmd.get('sent_ms')
    if not isinstance(sent, int) or now - sent > max_age_ms:
        return cmd, 'expired'
    return cmd, ''
