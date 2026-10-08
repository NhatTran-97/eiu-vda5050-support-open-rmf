from fastapi.testclient import TestClient

import pytest

from eiu_web_backend.app import create_app


@pytest.fixture
def app(settings, site, db, bridge):
    return create_app(settings, site, db, bridge, start_ticker=False)


def login(client, email):
    client.post("/api/v1/auth/login", json={"email": email, "password": "password123"})
    return {"origin": "http://testserver", "X-CSRF-Token": client.cookies["csrf_token"]}


def test_operators_have_no_administration(app):
    client = TestClient(app)
    headers = login(client, "student@eiu.edu.vn")
    me = client.get("/api/v1/auth/me").json()
    assert me["role"] == "operator" and me["allowedServices"] == ["delivery", "patrol"] and me["allZones"]
    assert "fleet.view" in me["permissions"] and "users.manage" not in me["permissions"]
    for path in ("/api/v1/admin/overview", "/api/v1/admin/users", "/api/v1/admin/integrations", "/api/v1/admin/settings",
                 "/api/v1/fleet/registration", "/api/v1/fleet/system"):
        r = client.get(path)
        assert r.status_code == 403 and r.json()["error"]["code"] == "ADMIN_ONLY", path
    assert client.get("/api/v1/fleet/robots").status_code == 200
    r = client.post("/api/v1/fleet/robots/tb3_1/pause", headers=headers)
    assert r.status_code == 403 and r.json()["error"]["code"] == "PERMISSION_DENIED"


def test_accounts_roles_and_audit(app):
    client = TestClient(app)
    headers = login(client, "admin@eiu.edu.vn")
    body = {"email": "New.Op@eiu.edu.vn", "fullName": "New Op", "role": "operator", "password": "longenough1",
            "department": "FabLab", "allowedServices": ["delivery", "cleaning"], "allowedZones": ["building_a"]}
    created = client.post("/api/v1/admin/users", headers=headers, json=body)
    assert created.status_code == 201 and created.json()["email"] == "new.op@eiu.edu.vn"
    assert created.json()["allowedServices"] == ["delivery", "cleaning"] and created.json()["department"] == "FabLab"
    assert client.post("/api/v1/admin/users", headers=headers, json=dict(body, email="y@eiu.edu.vn", allowedServices=["x"]))\
        .json()["error"]["code"] == "users.invalid_service"
    assert client.post("/api/v1/admin/users", headers=headers, json=body).json()["error"]["code"] == "users.email_taken"
    assert client.post("/api/v1/admin/users", headers=headers, json=dict(body, email="x@eiu.edu.vn", password="short"))\
        .json()["error"]["code"] == "auth.password_too_short"

    uid = created.json()["id"]
    r = client.patch(f"/api/v1/admin/users/{uid}", headers=headers, json={"role": "admin", "active": False})
    assert r.json()["role"] == "admin" and r.json()["active"] is False
    assert client.patch("/api/v1/admin/users/a1", headers=headers, json={"active": False}).json()["error"]["code"] == "users.self"
    assert client.post(f"/api/v1/admin/users/{uid}/password", headers=headers, json={"password": "anotherone1"}).status_code == 204

    roles = {r["role"]: r["permissions"] for r in client.get("/api/v1/admin/roles").json()}
    assert "users.manage" in roles["admin"] and "users.manage" not in roles["operator"]

    audit = client.get("/api/v1/admin/audit").json()
    actions = [i["action"] for i in audit["items"]]
    assert actions[:3] == ["user.password", "user.update", "user.create"]
    assert audit["items"][1]["detail"] == "role: operator → admin; disabled" and audit["items"][1]["actor"] == "admin"
    assert [i["action"] for i in client.get("/api/v1/admin/audit", params={"q": "new.op"}).json()["items"]] == actions[:3]


def test_overview_and_infrastructure(app):
    client = TestClient(app)
    login(client, "admin@eiu.edu.vn")
    overview = client.get("/api/v1/admin/overview").json()
    assert overview["users"] == {"active": 4, "admins": 1, "operators": 3} and overview["maps"]["levels"] == 1
    assert {"robots", "tasks", "alerts", "health", "fleets", "integrations", "recentChanges"} <= set(overview)
    infra = client.get("/api/v1/admin/infrastructure").json()
    assert {c["name"] for c in infra["chargers"]} == {"charger_1", "charger_2", "charger_3"}
    assert infra["doors"] == [] and infra["lifts"] == []
