import pytest
from fastapi.testclient import TestClient
from sqlalchemy import select, text

from eiu_web_backend import security
from eiu_web_backend.app import create_app
from eiu_web_backend.db import Database, Delivery, User, now_ms
from eiu_web_backend.services import FormError

ORIGIN = {"origin": "http://testserver"}
DELIVERY = {"serviceType": "delivery", "parameters": {"pickup": "library", "dropoff": "room_204", "itemType": "documents"}}


def robot(name, fleet="tb3_fleet", **extra):
    return {"name": name, "fleet": fleet, "level": "tb3_world", "x": 10.0, "y": -6.6, "yaw": 0.0, "battery": 80.0,
            "activity": "idle", "mode": "idle", "task_id": "", "path": [], "path_end_ms": None, "stale": False} | extra


@pytest.fixture
def app(settings, site, db, bridge):
    application = create_app(settings, site, db, bridge, start_ticker=False)
    application.state.service._robots = {"tb3_1": robot("tb3_1"), "cln_1": robot("cln_1", fleet="cleaner_fleet")}
    return application


def login(client, email="student@eiu.edu.vn"):
    assert client.post("/api/v1/auth/login", json={"email": email, "password": "password123"}).status_code == 200
    return {**ORIGIN, "X-CSRF-Token": client.cookies["csrf_token"]}


def set_user(db, uid, **fields):
    with db.session() as s:
        user = s.get(User, uid)
        for k, v in fields.items():
            setattr(user, k, v)
        s.commit()


def test_service_access_is_enforced_by_the_backend(app):
    client = TestClient(app)
    headers = login(client, "patrol@eiu.edu.vn")
    r = client.post("/api/v1/tasks", headers=headers, json=DELIVERY)
    assert r.status_code == 403
    assert r.json()["error"] == {"code": "SERVICE_NOT_ALLOWED",
                                 "message": "Bạn không có quyền tạo nhiệm vụ giao vận."}
    services = [s["id"] for s in client.get("/api/v1/services").json()]
    assert services == ["patrol"]


def test_service_form_is_validated(app):
    client = TestClient(app)
    headers = login(client)
    r = client.post("/api/v1/tasks", headers=headers, json=DELIVERY)
    assert r.status_code == 201, r.text
    task = r.json()
    assert task["service"] == "delivery" and task["state"] == "QUEUED" and task["parameters"]["itemType"] == "documents"
    assert task["zones"] == ["building_a"] and task["activity"][0]["type"] == "requested"
    bad = {"serviceType": "delivery", "parameters": {"pickup": "library", "dropoff": "library"}}
    assert client.post("/api/v1/tasks", headers=headers, json=bad).json()["error"]["code"] == "task.same_location"
    bad = {"serviceType": "delivery", "parameters": {"pickup": "library"}}
    assert client.post("/api/v1/tasks", headers=headers, json=bad).json()["error"] == {"code": "task.field_required",
                                                                                       "message": "dropoff"}
    patrol = {"serviceType": "patrol", "parameters": {"zone": "building_a", "route": "corridor_a", "rounds": 2}}
    r = client.post("/api/v1/tasks", headers=headers, json=patrol)
    assert r.status_code == 201 and r.json()["stops"][0]["id"] == "library" and r.json()["route"]["id"] == "corridor_a"


def test_zone_permission_and_switches(app, db):
    client = TestClient(app)
    set_user(db, "u1", allowed_zones=["other_zone"])
    headers = login(client)
    r = client.post("/api/v1/tasks", headers=headers, json=DELIVERY)
    assert r.status_code == 403 and r.json()["error"]["code"] == "ZONE_NOT_ALLOWED"

    set_user(db, "u1", allowed_zones=[], permissions=["fleet.view"])
    r = client.post("/api/v1/tasks", headers=headers, json=DELIVERY)
    assert r.status_code == 403 and r.json()["error"]["code"] == "PERMISSION_DENIED"

    client.post("/api/v1/auth/logout", headers=headers)
    admin = login(client, "admin@eiu.edu.vn")
    assert client.put("/api/v1/admin/toggles/zone/building_a", headers=admin, json={"enabled": False}).status_code == 200
    r = client.post("/api/v1/tasks", headers=admin, json=DELIVERY)
    assert r.status_code == 409 and r.json()["error"]["code"] == "ZONE_DISABLED"
    client.put("/api/v1/admin/toggles/zone/building_a", headers=admin, json={"enabled": True})
    client.put("/api/v1/admin/toggles/service/delivery", headers=admin, json={"enabled": False})
    assert client.post("/api/v1/tasks", headers=admin, json=DELIVERY).json()["error"]["code"] == "SERVICE_DISABLED"


def test_cleaning_needs_a_capable_fleet(app, db):
    client = TestClient(app)
    headers = login(client, "admin@eiu.edu.vn")
    body = {"serviceType": "cleaning", "parameters": {"area": "hall_a", "cleaningMode": "spot"}}
    r = client.post("/api/v1/tasks", headers=headers, json=body)
    assert r.status_code == 409 and r.json()["error"]["code"] == "NO_CAPABLE_FLEET"

    app.state.registry.fleets["cleaner_fleet"] = {"service": "cleaning", "capabilities": ["cleaning"]}
    r = client.post("/api/v1/tasks", headers=headers, json=body)
    assert r.status_code == 201, r.text
    envelope = app.state.service.rmf.sent[-1][1]
    assert envelope["request"]["category"] == "clean" and envelope["request"]["description"] == {"zone": "Patrol_C1"}
    assert "cleaningMode=spot" in envelope["request"]["labels"]


def test_robots_tasks_and_alerts_follow_services(app, db, bridge):
    app.state.registry.fleets["cleaner_fleet"] = {"service": "cleaning", "capabilities": ["cleaning"]}
    client = TestClient(app)
    login(client)
    robots = client.get("/api/v1/fleet/robots").json()
    assert [(r["name"], r["serviceType"], r["status"]) for r in robots] == [("tb3_1", "delivery", "IDLE")]
    overview = client.get("/api/v1/overview").json()
    assert [s["id"] for s in overview["services"]] == ["delivery", "patrol"]
    assert overview["robots"]["total"] == 1 and overview["robots"]["available"] == 1

    set_user(db, "u1", permissions=["fleet.view", "fleet.view_all"])
    assert len(client.get("/api/v1/fleet/robots").json()) == 2


def test_manual_assignment_and_reassignment(app, db, bridge):
    client = TestClient(app)
    headers = login(client)
    r = client.post("/api/v1/tasks", headers=headers, json=DELIVERY | {"robot": "tb3_1"})
    assert r.status_code == 403 and r.json()["error"]["code"] == "PERMISSION_DENIED"
    set_user(db, "u1", permissions=security.DEFAULT_OPERATOR + ["fleet.assign", "task.reassign"])
    r = client.post("/api/v1/tasks", headers=headers, json=DELIVERY | {"robot": "tb3_1"})
    assert r.status_code == 201
    envelope = bridge.sent[-1][1]
    assert envelope["type"] == "robot_task_request" and envelope["robot"] == "tb3_1" and envelope["fleet"] == "tb3_fleet"
    assert client.post("/api/v1/tasks", headers=headers, json=DELIVERY | {"robot": "cln_1"}).status_code == 409

    task_id = client.post("/api/v1/tasks", headers=headers, json=DELIVERY).json()["id"]
    with db.session() as s:
        s.get(Delivery, task_id).rmf_task_id = "delivery.dispatch-9"
        s.commit()
    r = client.post(f"/api/v1/tasks/{task_id}/reassign", headers=headers, json={"robot": "tb3_1"})
    assert r.status_code == 200 and r.json()["previousId"] == task_id and r.json()["requestedRobot"] == "tb3_1"
    assert [e[1]["type"] for e in bridge.sent[-2:]] == ["cancel_task_request", "robot_task_request"]


def test_pause_and_resume_go_through_rmf(app, db, bridge):
    set_user(db, "u1", permissions=security.DEFAULT_OPERATOR + ["task.pause"])
    client = TestClient(app)
    headers = login(client)
    task_id = client.post("/api/v1/tasks", headers=headers, json=DELIVERY).json()["id"]
    with db.session() as s:
        row = s.get(Delivery, task_id)
        row.rmf_task_id, row.status, row.rmf_state = "delivery.dispatch-3", "to_pickup", "underway"
        s.commit()
    assert client.post(f"/api/v1/tasks/{task_id}/pause", headers=headers).status_code == 200
    request_id, envelope = bridge.sent[-1]
    assert envelope == {"type": "interrupt_task_request", "task_id": "delivery.dispatch-3",
                        "labels": ["requester=eiu_web_dashboard"]}
    bridge.events.append(("response", request_id, {"success": True, "token": "tok-1"}))
    app.state.service.tick()
    task = client.get(f"/api/v1/tasks/{task_id}").json()
    assert task["state"] == "PAUSED" and task["actions"]["resume"]
    client.post(f"/api/v1/tasks/{task_id}/resume", headers=headers)
    assert bridge.sent[-1][1]["for_tokens"] == ["tok-1"]


def test_repeating_task_queues_the_next_one(app, db, bridge):
    client = TestClient(app)
    headers = login(client)
    start = now_ms() + 3_600_000
    body = {"serviceType": "patrol", "parameters": {"zone": "building_a", "route": "corridor_a"},
            "scheduledAt": start, "repeat": "daily"}
    task = client.post("/api/v1/tasks", headers=headers, json=body).json()
    assert task["state"] == "SCHEDULED" and task["repeat"] == "daily"
    assert client.post("/api/v1/tasks", headers=headers, json=dict(body, serviceType="delivery",
                                                                    parameters=DELIVERY["parameters"])).status_code == 422
    with db.session() as s:
        s.get(Delivery, task["id"]).scheduled_at = now_ms() - 1000
        s.commit()
    app.state.service.tick()
    with db.session() as s:
        nxt = s.scalars(select(Delivery).where(Delivery.previous_id == task["id"])).one()
        assert nxt.status == "scheduled" and nxt.scheduled_at >= now_ms() + 23 * 3_600_000


def test_access_editor_and_maintenance(app, db):
    client = TestClient(app)
    admin = login(client, "admin@eiu.edu.vn")
    catalog = client.get("/api/v1/admin/access-catalog").json()
    assert [s["id"] for s in catalog["services"]] == ["delivery", "cleaning", "patrol"]
    assert "task.create" in catalog["permissionGroups"][0]["permissions"]
    r = client.patch("/api/v1/admin/users/u3", headers=admin,
                     json={"allowedServices": ["patrol", "delivery"], "permissions": ["task.create", "fleet.view"]})
    assert r.json()["allowedServices"] == ["patrol", "delivery"]
    assert client.patch("/api/v1/admin/users/u3", headers=admin, json={"permissions": ["users.manage"]})\
        .json()["error"]["code"] == "users.invalid_permission"

    item = client.post("/api/v1/maintenance", headers=admin,
                       json={"robot": "tb3_1", "title": "Brush replacement", "dueAt": now_ms() - 1000}).json()
    table = client.get("/api/v1/maintenance").json()
    row = next(r for r in table["robots"] if r["robot"] == "tb3_1")
    assert row["status"] == "due" and row["issue"] == "Brush replacement"
    client.patch(f"/api/v1/maintenance/{item['id']}", headers=admin, json={"status": "in_progress"})
    assert next(r for r in client.get("/api/v1/fleet/robots").json() if r["name"] == "tb3_1")["status"] == "MAINTENANCE"

    client.post("/api/v1/auth/logout", headers=admin)
    operator = login(client)
    r = client.post("/api/v1/maintenance", headers=operator, json={"robot": "tb3_1", "title": "x"})
    assert r.status_code == 403 and r.json()["error"]["code"] == "ADMIN_ONLY"


def test_schedule_activity_and_search(app, db):
    client = TestClient(app)
    headers = login(client)
    start = now_ms() + 2 * 3_600_000
    client.post("/api/v1/tasks", headers=headers, json=DELIVERY | {"scheduledAt": start})
    items = client.get("/api/v1/schedule", params={"start": start - 60_000, "end": start + 60_000}).json()["items"]
    assert [(i["type"], i["status"], i["service"]) for i in items] == [("task", "scheduled", "delivery")]
    activity = client.get("/api/v1/analytics/activity", params={"range": "week"}).json()
    assert len(activity["buckets"]) == 7 and activity["buckets"][-1]["created"] == 1
    found = client.get("/api/v1/search", params={"q": "room 204"}).json()
    assert found["locations"][0]["id"] == "room_204" and found["users"] == []


def test_former_user_accounts_become_operators(tmp_path):
    import sqlite3

    path = tmp_path / "old.sqlite3"
    conn = sqlite3.connect(path)
    conn.executescript("""
        CREATE TABLE users (id VARCHAR(36) PRIMARY KEY, email VARCHAR(254), password_hash TEXT, full_name VARCHAR(200),
            role VARCHAR(20), locale VARCHAR(5), prefs JSON, active BOOLEAN, created_at INTEGER, last_login_at INTEGER);
        CREATE TABLE deliveries (id INTEGER PRIMARY KEY, requester_id VARCHAR(36), kind VARCHAR(16));
        INSERT INTO users VALUES ('x', 'x@eiu.edu.vn', 'h', 'X', 'user', 'vi', '{}', 1, 0, NULL);
        INSERT INTO deliveries VALUES (1, 'x', 'patrol');
    """)
    conn.close()
    again = Database.sqlite(path)
    with again.session() as s:
        user = s.get(User, "x")
        assert user.role == "operator" and user.allowed_services == ["delivery", "patrol"]
        assert security.can(user, "task.create") and not security.can(user, "users.manage")
        assert s.execute(text("SELECT service FROM deliveries")).scalar() == "patrol"


def test_form_errors_name_the_field(registry, site):
    svc = registry.get("patrol")
    with pytest.raises(FormError) as e:
        registry.validate(svc, {"zone": "building_a", "route": "corridor_a", "rounds": 99}, site)
    assert e.value.key == "rounds"
