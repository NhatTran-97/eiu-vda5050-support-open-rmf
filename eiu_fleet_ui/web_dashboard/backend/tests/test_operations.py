import json

import pytest
import yaml
from fastapi.testclient import TestClient

from eiu_web_backend.app import create_app
from eiu_web_backend.operations import check_graph, merge_discovery, suggest
from eiu_web_backend.service import ApiFail

REGISTRY = {"fleet": "tb3_fleet", "series": "TurtleBot3 Burger",
            "robots": [{"name": "tb3_1"}, {"name": "tb3_2"}, {"name": "tb3_3"}],
            "chargers": [{"name": "charger_1", "used_by": "tb3_1"}, {"name": "charger_2", "used_by": ""},
                         {"name": "charger_3", "used_by": "tb3_3"}]}


def test_suggest_continues_numbering_and_picks_a_free_charger():
    assert suggest({"series": "TurtleBot3 Burger"}, {"tb3_fleet": REGISTRY}) == \
        {"fleet": "tb3_fleet", "name": "tb3_4", "charger": "charger_2"}
    removed = {"removed_as": {"fleet": "tb3_fleet", "name": "tb3_2", "charger": "charger_2"}}
    assert suggest(removed, {"tb3_fleet": REGISTRY}) == {"fleet": "tb3_fleet", "name": "tb3_2", "charger": "charger_2"}


def test_merge_discovery_keeps_one_entry_per_robot():
    merged = merge_discovery({
        "a": {"snapshot": {"reporter": "a", "robots": [{"manufacturer": "M", "serial": "1"}]}},
        "b": {"snapshot": {"reporter": "b", "robots": [{"manufacturer": "M", "serial": "1",
                                                        "removed_as": {"fleet": "f"}}]}},
    })
    assert len(merged) == 1 and merged[0]["reporters"] == ["a", "b"] and merged[0]["removed_as"] == {"fleet": "f"}


def test_check_graph():
    v = [{"name": "a", "x": 0, "y": 0}, {"name": "b", "x": 1, "y": 0}]
    assert check_graph(v, [{"from": 0, "to": 1}]) == ("", "")
    assert check_graph(v + [{"name": "a", "x": 2, "y": 0}], [])[0] == "nav_graph.duplicate_name"
    assert check_graph(v, [{"from": 0, "to": 0}])[0] == "nav_graph.bad_lane"
    assert check_graph(v, [{"from": 0, "to": 1}, {"from": 0, "to": 1}])[0] == "nav_graph.duplicate_lane"


@pytest.fixture
def app(settings, site, db, bridge):
    application = create_app(settings, site, db, bridge, start_ticker=False)
    bridge.waiter = application.state.service.waiter
    application.state.service._robots = {"tb3_1": {
        "name": "tb3_1", "fleet": "tb3_fleet", "level": "tb3_world", "x": 1.0, "y": 2.0, "yaw": 0.0, "battery": 90.0,
        "activity": "idle", "task_id": "", "path": [], "path_end_ms": None, "stale": False}}
    return application


def login(client):
    client.post("/api/v1/auth/login", json={"email": "admin@eiu.edu.vn", "password": "password123"})
    return {"origin": "http://testserver", "X-CSRF-Token": client.cookies["csrf_token"]}


def test_robot_control_waits_for_the_adapter(app, bridge):
    client = TestClient(app)
    headers = login(client)
    bridge.responder = lambda t, i, b: ({"ok": True, "error": "", "message": "paused"}, False)
    r = client.post("/api/v1/fleet/robots/tb3_1/pause", headers=headers)
    assert r.json() == {"ok": True, "message": "paused"}
    assert bridge.commands[-1][0] == "robot_pause" and bridge.commands[-1][2] == {"robot": "tb3_1"}

    bridge.responder = lambda t, i, b: ({"ok": False, "error": "refused", "message": "robot busy"}, False)
    r = client.post("/api/v1/fleet/robots/tb3_1/speed-limit", headers=headers, json={"mps": 0.1})
    assert r.status_code == 409 and r.json() == {"error": {"code": "command.refused", "message": "robot busy"}}
    assert bridge.commands[-1][2] == {"robot": "tb3_1", "mps": 0.1}

    r = client.post("/api/v1/fleet/robots/tb3_1/init-position", headers=headers, json={"waypoint": "charger_1", "yaw": 0})
    assert bridge.commands[-1][0] == "robot_init_position" and round(bridge.commands[-1][2]["x"], 4) == 5.3683
    assert client.post("/api/v1/fleet/robots/ghost/pause", headers=headers).status_code == 404


def test_registration_waits_for_the_verdict(app, bridge):
    client = TestClient(app)
    headers = login(client)
    bridge.ops["registry"] = {"tb3_fleet": {"fleet": "tb3_fleet", "registry": REGISTRY}}
    bridge.ops["discovery"] = {"tb3_fleet": {"snapshot": {"reporter": "tb3_fleet", "robots": [
        {"manufacturer": "ROBOTIS", "serial": "0004", "series": "TurtleBot3 Burger"}]}}}
    view = client.get("/api/v1/fleet/registration").json()
    assert view["pending"][0]["suggestion"] == {"fleet": "tb3_fleet", "name": "tb3_4", "charger": "charger_2"}

    bridge.responder = lambda t, i, b: ({"ok": True, "result": {"ok": True, "dry_run": True, "warnings": [{"code": "w"}]}}, True)
    r = client.post("/api/v1/fleet/registration", headers=headers, json={
        "action": "check", "fleet": "tb3_fleet", "name": "tb3_4", "manufacturer": "ROBOTIS", "serial": "0004",
        "charger": "charger_2"})
    assert r.json()["ok"] and r.json()["dryRun"] and r.json()["warnings"] == [{"code": "w"}]
    assert bridge.commands[-1][2]["request"]["dry_run"] is True


def test_lanes_and_nav_graph(app, bridge, site):
    client = TestClient(app)
    headers = login(client)
    bridge.responder = lambda t, i, b: ({"ok": True, "error": "", "message": "sha-new"}, False)
    r = client.post("/api/v1/fleet/lanes", headers=headers, json={"close": [4, 5]})
    assert r.json() == {"ok": True, "fleets": ["tb3_fleet"]}
    assert bridge.commands[-1] [2] == {"fleet": "tb3_fleet", "close_lanes": [4, 5], "open_lanes": []}
    assert client.post("/api/v1/fleet/lanes", headers=headers, json={"close": [999]}).status_code == 422
    assert client.get("/api/v1/fleet/lanes").json()["offsets"] == {"tb3_world": 0}

    text = (site.levels["tb3_world"] and open(app.state.operations.settings.path("map", "nav_graph")).read())
    bridge.ops["nav_graph"] = {"path": "/x/nav_graph.yaml", "sha256": "s1", "yaml": text}
    graph = client.get("/api/v1/fleet/nav-graph").json()
    assert graph["available"] and graph["levelId"] == "tb3_world" and graph["vertices"][0]["name"] == "Patrol_A1"

    body = {"levelId": "tb3_world", "baseSha256": "s1", "vertices": graph["vertices"], "lanes": graph["lanes"]}
    body["vertices"].append({"name": "Patrol_new", "x": 12.0, "y": -7.0, "charger": False, "attrs": {}})
    body["lanes"].append({"from": 0, "to": len(body["vertices"]) - 1, "attrs": {}})
    assert client.put("/api/v1/fleet/nav-graph", headers=headers, json=body).json()["ok"]
    saved = yaml.safe_load(bridge.commands[-1][2]["yaml"])
    level = saved["levels"]["tb3_world"]
    assert level["vertices"][-1] == [12.0, -7.0, {"name": "Patrol_new"}]
    assert {"name": "charger_1", "is_charger": True} == level["vertices"][11][2]
    assert saved["building_name"] == "tb3_world" and level["lanes"][-1][:2] == [0, 14]

    body["vertices"][1]["name"] = "renamed"          # Patrol_B1 is the Library location's waypoint
    r = client.put("/api/v1/fleet/nav-graph", headers=headers, json=body)
    assert r.status_code == 422 and r.json()["error"]["code"] == "nav_graph.catalog_waypoint_missing"
    body["baseSha256"] = "old"
    assert client.put("/api/v1/fleet/nav-graph", headers=headers, json=body).json()["error"]["code"] == "nav_graph.stale"


def test_tick_follows_metrics_and_graph(app, bridge, site):
    ops = app.state.operations
    bridge.ops["adapters"] = {"fa": {"node": "fa", "robots": ["tb3_1"], "metrics_topic": True}}
    bridge.ops["metrics"] = {"fa": {"node": "fa", "received_ms": 1_000, "report": {"fleet": "tb3_fleet", "interval_s": 60,
                                                                                    "mqtt": {"connected": True}}}}
    assert ops.tick() is True
    assert ops.tick() is False
    system = TestClient(app)
    login(system)
    adapters = system.get("/api/v1/fleet/system").json()["adapters"]
    assert adapters[0]["node"] == "fa" and adapters[0]["fleet"] == "tb3_fleet"

    text = open(ops.settings.path("map", "nav_graph")).read().replace("10.498355712223173", "11.0")
    bridge.ops["nav_graph"] = {"sha256": "s2", "yaml": text}
    assert ops.tick() is True
    assert site.levels["tb3_world"].vertices[0]["x"] == 11.0


def test_operations_need_a_gateway(app, bridge):
    bridge.available = False
    with pytest.raises(ApiFail) as e:
        app.state.operations.control(type("U", (), {"id": "u1"})(), "tb3_1", "pause", {})
    assert e.value.code == "rmf.unavailable"
