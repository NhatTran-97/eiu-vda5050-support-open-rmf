import pytest
from fastapi.testclient import TestClient

from eiu_web_backend.app import create_app
from eiu_web_backend.db import Alert, Delivery, DeliveryEvent, now_ms
from eiu_web_backend.history import bucket

MIN = 60_000


def robot(name, **extra):
    return {"name": name, "fleet": "tb3_fleet", "level": "tb3_world", "x": 5.4, "y": -6.6, "yaw": 0.0,
            "battery": 80.0, "activity": "idle", "mode": "idle", "task_id": "", "path": [], "path_end_ms": None,
            "stale": False, **extra}


@pytest.fixture
def app(settings, site, db, bridge):
    application = create_app(settings, site, db, bridge, start_ticker=False)
    application.state.service._robots = {"tb3_1": robot("tb3_1"), "tb3_2": robot("tb3_2", mode="charging", battery=35.0)}
    return application


def client_for(app):
    client = TestClient(app)
    client.post("/api/v1/auth/login", json={"email": "admin@eiu.edu.vn", "password": "password123"})
    return client


def test_buckets():
    assert bucket("offline", True) == "offline" and bucket("charging", False) == "charging"
    assert bucket("idle", True) == "active" and bucket("moving", False) == "active" and bucket("idle", False) == "idle"


def test_history_samples_once_per_period(app):
    history, ops = app.state.history, app.state.operations
    now = now_ms()
    assert history.tick(ops.robots(), now) is True
    assert history.tick(ops.robots(), now + 1000) is False
    assert history.tick(ops.robots(), now + history.sample_ms) is True
    assert [b for _, b in history.battery("tb3_2", 1)] == [35.0, 35.0]
    today = history.utilization(1)[0]
    hours = history.sample_ms / 3_600_000
    assert today["idle"] == round(2 * hours, 2) and today["charging"] == round(2 * hours, 2)


def test_robot_details(app, db):
    with db.session() as s:
        row = Delivery(requester_id="u1", kind="delivery", pickup_id="fablab", dropoff_id="room_204", status="completed",
                       request_id="r1", robot="tb3_1", fleet="tb3_fleet", finished_at=now_ms())
        s.add(row)
        s.flush()
        s.add(DeliveryEvent(delivery_id=row.id, type="assigned", at=now_ms() - MIN, detail="tb3_1"))
        s.add(Alert(key="robot:tb3_1:battery", severity="warning", code="robot.battery_low", params={"robot": "tb3_1"}, robot="tb3_1"))
        s.commit()
    app.state.history.tick(app.state.operations.robots())
    client = client_for(app)
    body = client.get("/api/v1/admin/robots/tb3_1").json()
    assert body["robot"]["name"] == "tb3_1" and body["technical"]["nearest_waypoint"] == "charger_1"
    assert body["tasks"][0]["status"] == "completed" and len(body["battery"]) == 1 and len(body["utilization"]) == 7
    assert {e["source"] for e in body["events"]} == {"task", "alert"}
    assert client.get("/api/v1/admin/robots/ghost").status_code == 404


def test_analytics_report(app, db):
    now = now_ms()
    with db.session() as s:
        for i, status in enumerate(("completed", "completed", "failed", "cancelled")):
            row = Delivery(requester_id="u1", kind="delivery", pickup_id="fablab", dropoff_id="room_204", status=status,
                           request_id=f"r{i}", created_at=now - 20 * MIN, finished_at=now - 5 * MIN)
            s.add(row)
            s.flush()
            s.add(DeliveryEvent(delivery_id=row.id, type="assigned", at=now - 18 * MIN, detail=""))
        s.add(Alert(key="k", severity="critical", code="rmf.unavailable", params={}))
        s.commit()
    client = client_for(app)
    report = client.get("/api/v1/admin/analytics", params={"days": 7}).json()
    k = report["kpis"]
    assert (k["created"], k["completed"], k["failed"], k["cancelled"]) == (4, 2, 1, 1)
    assert k["successRate"] == 66.7 and k["avgDurationMin"] == 15.0 and k["avgWaitMin"] == 2.0 and k["alerts"] == 1
    assert len(report["tasksPerDay"]) == 7 and report["tasksPerDay"][-1]["completed"] == 2
    assert report["topPlaces"][0]["id"] == "room_204" and report["topPlaces"][0]["count"] == 4
    assert sum(b["robots"] for b in report["battery"]) == 2
    assert client.get("/api/v1/admin/analytics", params={"days": 5}).status_code == 422


def test_tasks_watch_lists(app, db):
    now = now_ms()
    with db.session() as s:
        s.add(Delivery(requester_id="u1", kind="delivery", pickup_id="fablab", dropoff_id="room_204", status="queued",
                       request_id="q", created_at=now - 3 * MIN))
        s.add(Delivery(requester_id="u1", kind="delivery", pickup_id="fablab", dropoff_id="room_204", status="in_transit",
                       request_id="l", robot="tb3_1", eta_at=now - 10 * MIN))
        s.commit()
    body = client_for(app).get("/api/v1/fleet/tasks").json()
    assert [t["robot"] for t in body["unassigned"]] == [None]
    assert body["late"][0]["robot"] == "tb3_1" and body["late"][0]["lateMin"] >= 10
    assert body["summary"]["active"] == 2 and body["summary"]["late"] == 1
