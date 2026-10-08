from eiu_web_backend.db import Delivery, DeliveryEvent, Notification, User
from eiu_web_backend.access import Access
from eiu_web_backend.service import Service, derive_status
from eiu_web_backend.services import Registry
from sqlalchemy import select


def test_derive_status():
    assert derive_status("delivery", "queued", 0, None, 0, None, False) == "queued"
    assert derive_status("delivery", "queued", 0, 10_000, 0, None, False) == "scheduled"
    assert derive_status("delivery", "underway", 1, None, 0, None, False) == "to_pickup"
    assert derive_status("delivery", "underway", 1, None, 0, "pickup", False) == "at_pickup"
    assert derive_status("delivery", "underway", 2, None, 0, None, False) == "in_transit"
    assert derive_status("delivery", "underway", 2, None, 0, "dropoff", False) == "arrived"
    assert derive_status("patrol", "underway", 3, None, 0, None, False) == "underway"
    assert derive_status("patrol", "cancelled", 3, None, 0, None, False) == "cancelled"


def make(settings, site, db, bridge, body, user_id="u1"):
    registry = Registry.load(settings.path("site", "services"), site)
    service = Service(settings, site, db, bridge, registry, Access(registry, site, db))
    with db.session() as s:
        row = service.create(s, s.get(User, user_id), body)
        return service, row.id


def robot(name, task_id, x=10.0, y=-6.6):
    return {"name": name, "fleet": "tb3_fleet", "level": "L1", "x": x, "y": y, "yaw": 0.0, "battery": 80.0,
            "activity": "moving", "task_id": task_id, "path": [(15.0, -6.6)], "path_end_ms": None, "stale": False}


def test_delivery_follows_rmf_sources(settings, site, db, bridge):
    service, did = make(settings, site, db, bridge,
                        {"kind": "delivery", "pickupId": "fablab", "dropoffId": "room_204", "packageType": "food"})
    request_id, envelope = bridge.sent[0]
    assert envelope["request"]["category"] == "delivery"
    assert envelope["request"]["description"]["pickup"]["place"] == "Patrol_A2"

    bridge.events.append(("response", request_id, {"success": True, "state": {"booking": {"id": "delivery.dispatch-1"}}}))
    bridge.events.append(("dispatch", [{"task_id": "delivery.dispatch-1", "status": 3, "robot": "tb3_1",
                                        "fleet": "tb3_fleet", "errors": []}]))
    changed = service.tick()
    assert changed["u1"] == {"deliveries", "notifications"}

    bridge.robots = {"tb3_1": robot("tb3_1", "delivery.dispatch-1")}
    service.tick()
    with db.session() as s:
        assert s.get(Delivery, did).status == "to_pickup"
    live = service.robots_live()
    assert live[0]["deliveryId"] == did and live[0]["levelId"] == "tb3_world" and live[0]["remainingM"] == 5.0
    bridge.robots["tb3_1"]["path"] = []
    service.tick()
    computed = service.robots_live()[0]
    assert computed["path"][-1] == [10.454, -8.209] and computed["remainingM"] > 0
    assert service.owner_of(did) == "u1"

    bridge.workcells = {"mock_dispenser_1": {"kind": "dispenser", "busy": True, "seconds": 3.0}}
    service.tick()
    bridge.workcells = {"mock_dispenser_1": {"kind": "dispenser", "busy": False, "seconds": 0.0}}
    service.tick()
    with db.session() as s:
        assert s.get(Delivery, did).status == "in_transit"

    bridge.robots = {"tb3_1": robot("tb3_1", "")}
    service.tick()
    with db.session() as s:
        row = s.get(Delivery, did)
        assert row.status == "completed" and row.finished_at is not None
        events = {e.type for e in s.scalars(select(DeliveryEvent).where(DeliveryEvent.delivery_id == did))}
        assert {"requested", "assigned", "arrived_pickup", "picked_up", "completed"} <= events
        kinds = [n.type for n in s.scalars(select(Notification).where(Notification.user_id == "u1"))]
        assert kinds[0] == "request_received" and "delivered" in kinds


def test_events_outrank_fleet_guess(settings, site, db, bridge):
    service, did = make(settings, site, db, bridge, {"kind": "patrol", "stops": ["library", "cafeteria"], "rounds": 2})
    request_id, envelope = bridge.sent[0]
    assert envelope["request"] == {"category": "patrol", "description": {"places": ["Patrol_B1", "Patrol_E1"], "rounds": 2},
                                   "unix_millis_earliest_start_time": 0, "requester": "eiu_web_dashboard"}
    bridge.events.append(("response", request_id, {"type": "dispatch_task_response", "task_id": "patrol-7", "success": True}))
    service.tick()
    bridge.robots = {"r": robot("r", "patrol-7")}
    service.tick()
    bridge.robots = {"r": robot("r", "")}
    service.tick()
    bridge.task_event({"booking": {"id": "patrol-7"}, "status": "failed"})
    service.tick()
    with db.session() as s:
        assert s.get(Delivery, did).status == "failed"


def test_patrol_rounds_from_task_events(settings, site, db, bridge):
    service, did = make(settings, site, db, bridge, {"kind": "patrol", "stops": ["library", "cafeteria"], "rounds": 2})
    request_id, _ = bridge.sent[0]
    bridge.events.append(("response", request_id, {"success": True, "state": {"booking": {"id": "p-1"}}}))
    bridge.task_event({"booking": {"id": "p-1"}, "status": "underway", "assigned_to": {"group": "tb3_fleet", "name": "r"},
                       "active": 3, "completed": [1, 2], "pending": [4]})
    service.tick()
    with db.session() as s:
        row = s.get(Delivery, did)
        assert (row.status, row.rounds_done, row.robot) == ("underway", 1, "r")


def test_dispatch_without_answer_fails(settings, site, db, bridge):
    service, did = make(settings, site, db, bridge, {"kind": "delivery", "pickupId": "library", "dropoffId": "room_118"})
    with db.session() as s:
        row = s.get(Delivery, did)
        row.created_at -= 60_000
        s.commit()
    service.tick()
    with db.session() as s:
        row = s.get(Delivery, did)
        assert row.status == "failed" and row.error == "dispatch.no_response"


def test_refused_command_fails_the_task(settings, site, db, bridge):
    service, did = make(settings, site, db, bridge, {"kind": "delivery", "pickupId": "library", "dropoffId": "room_118"})
    request_id, _ = bridge.sent[0]
    bridge.events.append(("command_result", request_id, False, "bad_body", ""))
    service.tick()
    with db.session() as s:
        row = s.get(Delivery, did)
        assert (row.status, row.error) == ("failed", "gateway.bad_body")


def test_unreachable_gateway_fails_at_once(settings, site, db, bridge):
    bridge.send = lambda *_: False
    service, did = make(settings, site, db, bridge, {"kind": "delivery", "pickupId": "library", "dropoffId": "room_118"})
    with db.session() as s:
        row = s.get(Delivery, did)
        assert (row.status, row.error) == ("failed", "gateway.unreachable")
