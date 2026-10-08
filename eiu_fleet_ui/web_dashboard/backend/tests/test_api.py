import pytest
from fastapi.testclient import TestClient

from eiu_web_backend.app import create_app

ORIGIN = {"origin": "http://testserver"}


@pytest.fixture
def client(settings, site, db, bridge):
    return TestClient(create_app(settings, site, db, bridge, start_ticker=False))


def login(client, email="student@eiu.edu.vn"):
    r = client.post("/api/v1/auth/login", json={"email": email, "password": "password123"})
    assert r.status_code == 200
    return {**ORIGIN, "X-CSRF-Token": client.cookies["csrf_token"]}


def test_requires_session(client):
    assert client.get("/api/v1/deliveries").json() == {"error": {"code": "auth.required"}}
    assert client.get("/api/v1/config").status_code == 200


def test_login_lockout(client, settings):
    for _ in range(settings.get("auth", "lock_failures")):
        assert client.post("/api/v1/auth/login", json={"email": "student@eiu.edu.vn", "password": "x"}).status_code == 401
    assert client.post("/api/v1/auth/login", json={"email": "student@eiu.edu.vn", "password": "password123"}).status_code == 429


def test_writes_need_csrf(client):
    login(client)
    r = client.post("/api/v1/deliveries", json={"kind": "delivery", "pickupId": "fablab", "dropoffId": "room_204"})
    assert r.json()["error"]["code"] == "auth.csrf"


def test_create_list_and_isolation(client, bridge):
    headers = login(client)
    r = client.post("/api/v1/deliveries", headers=headers,
                    json={"kind": "delivery", "pickupId": "fablab", "dropoffId": "room_204", "packageType": "documents"})
    assert r.status_code == 201, r.text
    d = r.json()
    assert d["status"] == "queued" and d["kind"] == "delivery" and d["actions"]["cancel"] is True
    assert [s["key"] for s in d["timeline"]] == ["requested", "picked_up", "en_route", "arriving", "delivered"]
    assert len(bridge.sent) == 1

    listing = client.get("/api/v1/deliveries?group=active").json()
    assert listing["counts"]["active"] == 1 and listing["items"][0]["id"] == d["id"]

    r = client.post("/api/v1/deliveries", headers=headers,
                    json={"kind": "patrol", "stops": ["library"], "rounds": 3})
    assert r.json()["error"]["code"] == "patrol.needs_two_stops"
    r = client.post("/api/v1/deliveries", headers=headers,
                    json={"kind": "delivery", "pickupId": "fablab", "dropoffId": "fablab"})
    assert r.json()["error"]["code"] == "delivery.same_location"

    client.post("/api/v1/auth/logout", headers=headers)
    login(client, "other@eiu.edu.vn")
    assert client.get(f"/api/v1/deliveries/{d['id']}").status_code == 200
    assert client.get("/api/v1/deliveries").json()["counts"]["all"] == 0

    client.post("/api/v1/auth/logout", headers=headers)
    patrol = login(client, "patrol@eiu.edu.vn")
    assert client.get(f"/api/v1/deliveries/{d['id']}").status_code == 404
    assert client.post(f"/api/v1/deliveries/{d['id']}/cancel", headers=patrol).status_code == 404

    client.post("/api/v1/auth/logout", headers=patrol)
    login(client, "admin@eiu.edu.vn")
    assert client.get(f"/api/v1/deliveries/{d['id']}").status_code == 200


def test_open_task_limit(client, settings):
    headers = login(client)
    body = {"kind": "patrol", "stops": ["library", "cafeteria"], "rounds": 1}
    for _ in range(settings.get("delivery", "max_open")):
        assert client.post("/api/v1/deliveries", headers=headers, json=body).status_code == 201
    assert client.post("/api/v1/deliveries", headers=headers, json=body).json()["error"]["code"] == "delivery.limit_reached"


def test_site_endpoints(client):
    login(client)
    levels = client.get("/api/v1/levels").json()
    assert levels[0]["id"] == "tb3_world"
    image = client.get(levels[0]["imageUrl"])
    assert image.status_code == 200 and "immutable" in image.headers["cache-control"]
    assert len(client.get("/api/v1/locations").json()) == 8
    assert client.get("/api/v1/levels/tb3_world/graph").json()["vertices"][0]["name"] == "Patrol_A1"


def test_cross_origin_write_rejected(client):
    headers = login(client)
    r = client.put("/api/v1/me/preferences", headers={**headers, "origin": "http://evil.example"}, json={"locale": "en"})
    assert r.status_code == 403
    assert client.put("/api/v1/me/preferences", headers=headers, json={"locale": "en"}).json()["locale"] == "en"
