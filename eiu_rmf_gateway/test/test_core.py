import json

from eiu_rmf_gateway import contract
from eiu_rmf_gateway.core import Gateway, Recorder


class FakeRedis:
    """The Redis calls the gateway makes, kept in dicts."""

    def __init__(self):
        self.streams = {}
        self.hashes = {}
        self.strings = {}
        self.acked = []
        self.delivered = []

    def xgroup_create(self, name, group, id='0', mkstream=False):
        if ('group', name) in self.strings:
            raise Exception('BUSYGROUP Consumer Group name already exists')
        self.strings[('group', name)] = group
        self.streams.setdefault(name, [])

    def xadd(self, name, fields, maxlen=None, approximate=True):
        entries = self.streams.setdefault(name, [])
        entry_id = f'{len(entries) + 1}-0'
        entries.append((entry_id, dict(fields)))
        return entry_id

    def xreadgroup(self, group, consumer, streams, count=None, block=None):
        (name, start), = streams.items()
        if start == '0':
            entries = [e for e in self.streams.get(name, []) if e[0] in self.delivered and e[0] not in self.acked]
        else:
            entries = [e for e in self.streams.get(name, []) if e[0] not in self.delivered]
            self.delivered += [e[0] for e in entries]
        return [(name, entries)] if entries else []

    def xack(self, name, group, entry_id):
        self.acked.append(entry_id)

    def hset(self, name, mapping):
        self.hashes.setdefault(name, {}).update(mapping)

    def set(self, name, value, px=None):
        self.strings[name] = (value, px)

    def pipeline(self):
        return self

    def execute(self):
        return []


def make():
    redis, published = FakeRedis(), []

    def execute(cmd_type, cmd_id, body):
        if cmd_type == 'robot_pause':
            published.append((cmd_id, body))
            return None
        published.append((cmd_id, body))
        return True, '', ''

    gw = Gateway(redis, contract.Keys('t'), Recorder(), execute,
                 events_maxlen=100, command_max_age_s=30, heartbeat_ttl_s=3, info={'ros_domain_id': '7'})
    gw.ensure_group()
    gw.ensure_group()
    return gw, redis, published


def command(cmd_id, body, sent_ms=None, cmd_type='task_request'):
    return {'msg': json.dumps({'v': 1, 'id': cmd_id, 'type': cmd_type, 'sent_ms': sent_ms or contract.now_ms(), 'body': body})}


def events(redis):
    return [json.loads(f['msg']) for _, f in redis.streams.get('t:events', [])]


def test_task_request_is_published_and_acknowledged():
    gw, redis, published = make()
    body = {'type': 'dispatch_task_request', 'request': {'category': 'patrol', 'description': {'places': ['A'], 'rounds': 1}}}
    redis.xadd('t:commands', command('c1', body))
    assert gw.handle_commands(0) == 0          # the first read only collects pending entries
    assert gw.handle_commands(0) == 1
    gw.flush()
    assert published == [('c1', body)]
    assert redis.acked == ['1-0']
    assert events(redis)[0]['body'] == {'id': 'c1', 'ok': True, 'error': '', 'message': ''}


def test_bad_commands_are_refused_not_published():
    gw, redis, published = make()
    redis.xadd('t:commands', command('c1', {'type': 'publish_anything'}))
    redis.xadd('t:commands', command('c2', {'type': 'cancel_task_request'}, cmd_type='shell'))
    redis.xadd('t:commands', command('c3', {'type': 'cancel_task_request'}, sent_ms=contract.now_ms() - 60_000))
    redis.xadd('t:commands', {'msg': 'not json'})
    gw.handle_commands(0)
    gw.handle_commands(0)
    gw.flush()
    assert published == []
    assert [e['body']['error'] for e in events(redis)] == ['bad_body', 'unknown_type', 'expired', 'bad_json']
    assert len(redis.acked) == 4


def test_flush_writes_changed_state_and_heartbeat():
    gw, redis, _ = make()
    gw.recorder.set_fleet('tb3_fleet', [{'name': 'tb3_1', 'x': 1.0}])
    gw.recorder.set_workcell('mock_dispenser_1', 'dispenser', True, 3.0)
    gw.recorder.add_event('dispatch_states', {'states': []})
    gw.flush()
    fleet = json.loads(redis.hashes['t:fleets']['tb3_fleet'])
    assert fleet['robots'][0]['name'] == 'tb3_1' and fleet['v'] == 1 and fleet['received_ms'] > 0
    assert json.loads(redis.hashes['t:workcells']['mock_dispenser_1'])['busy'] is True
    beat, ttl = redis.strings['t:gateway']
    assert json.loads(beat)['ros_domain_id'] == '7' and ttl == 3000
    assert events(redis)[0]['type'] == 'dispatch_states'

    redis.hashes.clear()
    gw.recorder.set_workcell('mock_dispenser_1', 'dispenser', True, 3.0)
    gw.flush()
    assert 't:workcells' not in redis.hashes   # unchanged workcell is not rewritten


def test_pending_command_of_a_crashed_gateway_is_handled_once():
    gw, redis, published = make()
    body = {'type': 'cancel_task_request', 'task_id': 'x'}
    redis.xadd('t:commands', command('c1', body))
    redis.delivered.append('1-0')                # delivered to a gateway that died before acknowledging
    assert gw.handle_commands(0) == 1
    assert gw.handle_commands(0) == 0
    assert published == [('c1', body)]


def test_robot_command_reports_later_and_bad_bodies_are_refused():
    gw, redis, published = make()
    redis.xadd('t:commands', command('p1', {'robot': 'tb3_1'}, cmd_type='robot_pause'))
    redis.xadd('t:commands', command('s1', {'robot': 'tb3_1', 'mps': -1}, cmd_type='robot_speed_limit'))
    redis.xadd('t:commands', command('l1', {'fleet': 'tb3_fleet', 'close_lanes': [1, 'x']}, cmd_type='lane_request'))
    redis.xadd('t:commands', command('r1', {'request': {'action': 'add', 'fleet': 'f', 'name': 'n'}}, cmd_type='registration_request'))
    gw.handle_commands(0)
    gw.handle_commands(0)
    gw.flush()
    assert [p[0] for p in published] == ['p1', 'r1']
    results = {e['body']['id']: e['body'] for e in events(redis)}
    assert 'p1' not in results                       # the ROS side answers later
    assert results['s1']['error'] == 'bad_body' and results['l1']['error'] == 'bad_body'
    assert results['r1']['ok'] is True
    gw.recorder.command_result('p1', True, '', 'paused')
    gw.flush()
    assert events(redis)[-1]['body'] == {'id': 'p1', 'ok': True, 'error': '', 'message': 'paused'}


def test_states_are_written_only_when_they_change():
    gw, redis, _ = make()
    gw.recorder.put('controls', 'tb3_1', {'robot': 'tb3_1', 'speed_limit': 0.0})
    gw.recorder.put('lanes', 'tb3_fleet', {'fleet': 'tb3_fleet', 'closed_lanes': [3], 'received_ms': 1})
    gw.recorder.set_value('nav_graph', {'sha256': 'abc'})
    gw.flush()
    assert json.loads(redis.hashes['t:controls']['tb3_1'])['speed_limit'] == 0.0
    assert json.loads(redis.strings['t:nav_graph'][0])['sha256'] == 'abc'
    redis.hashes.clear()
    gw.recorder.put('lanes', 'tb3_fleet', {'fleet': 'tb3_fleet', 'closed_lanes': [3], 'received_ms': 2})
    gw.flush()
    assert 't:lanes' not in redis.hashes
