import hashlib

from eiu_rmf_gateway import nav_graph

GRAPH = """building_name: b
levels:
  L1:
    vertices:
    - [0.0, 0.0, {name: a}]
    - [1.0, 0.0, {name: b}]
    lanes:
    - [0, 1, {}]
"""


def test_save_checks_version_and_keeps_a_backup(tmp_path):
    path = tmp_path / 'nav_graph.yaml'
    path.write_text(GRAPH)
    base = nav_graph.read(path)
    edited = GRAPH.replace('{name: b}', '{name: c}')
    ok, error, sha = nav_graph.save(path, edited, base['sha256'])
    assert ok and sha == hashlib.sha256(edited.encode()).hexdigest()
    assert path.read_text() == edited and (tmp_path / 'nav_graph.yaml.bak').read_text() == GRAPH
    assert nav_graph.save(path, GRAPH, base['sha256']) == (False, 'stale', 'the nav graph changed since it was read')


def test_check_refuses_broken_graphs():
    assert nav_graph.check(GRAPH) == ''
    assert nav_graph.check('levels: {}') == 'no_levels'
    assert nav_graph.check(GRAPH.replace('[0, 1, {}]', '[0, 5, {}]')) == 'bad_lane'
    assert nav_graph.check('levels: [') == 'bad_yaml'
