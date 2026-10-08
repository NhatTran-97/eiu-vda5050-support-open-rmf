import time

import pytest
from fastapi.testclient import TestClient

from eiu_web_backend.app import create_app
from eiu_web_backend.db import Delivery, now_ms


def robot(name, **extra):
    return {"name": name, "fleet": "tb3_fleet", "level": "tb3_world", "x": 1.0, "y": 2.0, "yaw": 0.0,
            "battery": 90.0, "activity": "idle", "mode": "idle", "task_id": "", "path": [], "path_end_ms": None,
            "stale": False, **extra}


@pytest.fixture
def app(settings, site, db, bridge):
    settings.data["alerts"]["open_after_s"] = 0.0
    application = create_app(settings, site, db, bridge, start_ticker=False)
    application.state.service._robots = {"tb3_1": robot("tb3_1"), "tb3_2": robot("tb3_2")}
    return application


def login(client):
    client.post("/api/v1/auth/login", json={"email": "admin@eiu.edu.vn", "password": "password123"})
    return {"origin": "http://testserver", "X-CSRF-Token": client.cookies["csrf_token"]}


def add_task(db, status, **extra):
    with db.session() as s:
        row = Delivery(requester_id="u2", kind="delivery", pickup_id="fablab", dropoff_id="room_204", status=status,
                       request_id=f"r-{status}-{now_ms()}", rmf_state="queued", **extra)
        s.add(row)
        s.commit()
        return row.id


def test_condition_alerts_open_escalate_and_close(app, db):
    alerts = app.state.alerts
    robots = {"tb3_1": robot("tb3_1", stale=True), "tb3_2": robot("tb3_2", battery=15.0)}
    assert alerts.tick("online", robots, []) is True
    items = alerts.list("open")["items"]
    assert [(a["code"], a["severity"]) for a in items] == [("robot.offline", "critical"), ("robot.battery_low", "warning")]
    assert alerts.tick("online", robots, []) is False

    robots["tb3_2"]["battery"] = 5.0
    assert alerts.tick("online", robots, []) is True
    battery = next(a for a in alerts.list("open")["items"] if a["code"] == "robot.battery_low")
    assert battery["severity"] == "critical" and battery["params"]["battery"] == 5

    robots = {"tb3_1": robot("tb3_1"), "tb3_2": robot("tb3_2", mode="charging", battery=5.0)}
    alerts.tick("online", robots, [])
    assert alerts.list("open")["items"] == []
    closed = alerts.list("all")["items"]
    assert len(closed) == 2 and all(a["resolvedAt"] and a["resolvedBy"] is None for a in closed)


def test_task_alerts_and_operator_actions(app, db, settings):
    old = now_ms() - int(settings.get("alerts", "task_waiting_s") * 1000) - 60_000
    waiting = add_task(db, "queued", created_at=old)
    failed = add_task(db, "failed", finished_at=now_ms(), error="dispatch.refused")
    alerts = app.state.alerts
    alerts.tick("unavailable", {}, [{"key": "adapter:fa:mqtt", "severity": "critical", "title": "MQTT lost", "detail": ""}])
    codes = {a["code"]: a for a in alerts.list("open")["items"]}
    assert set(codes) == {"rmf.unavailable", "task.waiting", "task.failed", "adapter.attention"}
    assert codes["task.waiting"]["deliveryId"] == waiting and codes["task.failed"]["params"]["id"] == failed

    client = TestClient(app)
    headers = login(client)
    r = client.post(f"/api/v1/fleet/alerts/{codes['rmf.unavailable']['id']}/resolve", headers=headers)
    assert r.status_code == 409 and r.json()["error"]["code"] == "alert.still_active"
    assert client.post(f"/api/v1/fleet/alerts/{codes['task.failed']['id']}/resolve", headers=headers).status_code == 204
    assert client.post("/api/v1/fleet/alerts/ack-all", headers=headers).json() == {"acknowledged": 3}
    body = client.get("/api/v1/fleet/alerts").json()
    assert body["counts"] == {"open": 3, "unacked": 0, "critical": 2}
    assert all(a["ackedBy"] == "admin" for a in body["items"])

    alerts.tick("unavailable", {}, [])
    assert "task.failed" not in {a["code"] for a in alerts.list("open")["items"]}


def test_a_silent_fleet_is_one_alert(app):
    alerts = app.state.alerts
    alerts.tick("offline", {"tb3_1": robot("tb3_1", stale=True), "tb3_2": robot("tb3_2", stale=True)}, [])
    [alert] = alerts.list("open")["items"]
    assert alert["code"] == "fleet.offline" and alert["params"] == {"fleet": "tb3_fleet", "robots": 2}


def test_short_conditions_do_not_open_alerts(app, settings):
    settings.data["alerts"]["open_after_s"] = 60.0
    alerts = app.state.alerts
    assert alerts.tick("unavailable", {}, []) is False
    assert alerts.tick("online", {}, []) is False
    assert alerts.list("all")["items"] == []


def test_adapter_hearing_fewer_robots(app):
    adapter = {"node": "fa", "fleet": "tb3_fleet", "status": "ok",
               "robots": {"registered": 3, "online": 2, "oldest_state_robot": "tb3_3", "state_age_max_s": 42.4}}
    alerts = app.state.alerts
    alerts.tick("online", {}, [], [adapter])
    [alert] = alerts.list("open")["items"]
    assert alert["code"] == "adapter.robots_offline" and alert["robot"] == "tb3_3" and alert["params"]["age"] == 42
    alerts.tick("online", {}, [], [dict(adapter, status="silent")])
    assert alerts.list("open")["items"] == []


def test_overview_and_tasks(app, db, bridge):
    add_task(db, "queued")
    add_task(db, "scheduled", scheduled_at=now_ms() + 3_600_000)
    done = add_task(db, "completed", finished_at=now_ms())
    client = TestClient(app)
    headers = login(client)
    overview = client.get("/api/v1/fleet/overview").json()
    assert overview["robots"]["total"] == 2 and overview["robots"]["online"] == 2
    assert overview["tasks"]["queued"] == 1 and overview["tasks"]["scheduled"] == 1
    assert overview["tasks"]["completedToday"] == 1
    assert {h["key"] for h in overview["health"]} >= {"database", "gateway", "rmf", "fleet"}

    tasks = client.get("/api/v1/fleet/tasks", params={"group": "all"}).json()
    assert tasks["counts"] == {"active": 1, "scheduled": 1, "finished": 1, "all": 3}
    assert all(t["requester"]["fullName"] == "other" for t in tasks["items"])
    assert [t["id"] for t in client.get("/api/v1/fleet/tasks", params={"group": "all", "q": f"#{done}"}).json()["items"]] == [done]

    scheduled = next(t for t in tasks["items"] if t["status"] == "scheduled")
    assert scheduled["actions"]["cancel"] and not scheduled["actions"]["track"]
    r = client.post(f"/api/v1/fleet/tasks/{scheduled['id']}/cancel", headers=headers)
    assert r.status_code == 409 and r.json()["error"]["code"] == "delivery.not_dispatched_yet"
    assert client.get("/api/v1/fleet/tasks", params={"group": "bad"}).status_code == 422


def test_robot_the_adapter_no_longer_hears_is_offline(app):
    ops = app.state.operations
    ops.metrics.expect("fa")
    ops.metrics.set_present("fa", True)
    ops.metrics.apply("fa", {"fleet": "tb3_fleet", "interval_s": 5, "mqtt": {"connected": True},
                             "robots": {"registered": 2, "online": 1, "oldest_state_robot": "tb3_2",
                                        "state_age_max_s": 30, "state_timeout_s": 10}}, time.time())
    assert {r["name"]: r["mode"] for r in ops.robots()} == {"tb3_1": "idle", "tb3_2": "offline"}
    assert ops.overview()["robots"]["online"] == 1
