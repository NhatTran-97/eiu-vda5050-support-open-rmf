"""FastAPI application: REST API under /api/v1 and the browser WebSocket /ws."""

import asyncio
import contextlib
import logging
from urllib.parse import urlparse

from fastapi import Body, Depends, FastAPI, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from sqlalchemy import select, update
from sqlalchemy.orm import Session

from . import security, views
from .access import Access
from .db import Database, Delivery, DeliveryEvent, Notification, SavedLocation, User, now_ms, user_by_email
from .admin import Admin
from .analytics import Analytics
from .dashboard import Dashboard
from .errors import ApiFail
from .history import History
from .alerts import Alerts
from .maintenance import Maintenance
from .operations import Operations
from .realtime import Connection, Hub, run_ticker
from .service import Service
from .services import Registry
from .settings import Settings
from .site import Site

log = logging.getLogger("eiu_web.app")
API = "/api/v1"
LOCALES = ("vi", "en")
ACTIVE_WRITE_MS = 60_000
SEARCH_LIMIT = 6


def create_app(settings: Settings, site: Site, db: Database, rmf, start_ticker: bool = True,
               registry: Registry | None = None) -> FastAPI:
    registry = registry or Registry.load(settings.path("site", "services"), site)
    access = Access(registry, site, db)
    service = Service(settings, site, db, rmf, registry, access)
    alerts = Alerts(settings, db)
    history = History(settings, db)
    maintenance = Maintenance(settings, db, history)
    operations = Operations(settings, site, db, rmf, service, alerts, maintenance)
    admin = Admin(settings, site, db, operations, history, access)
    analytics = Analytics(settings, site, db, operations, history)
    dashboard = Dashboard(settings, site, db, service, operations, alerts, maintenance, history, access)
    period_s = settings.get("realtime", "period_s")
    hub = Hub(int(period_s * 1000))
    limiter = security.LoginLimiter(settings.get("auth", "lock_failures"), settings.get("auth", "lock_s"))
    secure_cookies = settings.get("server", "secure_cookies")

    @contextlib.asynccontextmanager
    async def lifespan(_app):
        task = asyncio.create_task(run_ticker(service, operations, alerts, history, hub, period_s, maintenance)) \
            if start_ticker else None
        yield
        if task:
            task.cancel()

    app = FastAPI(title="EIU Robot Services API", lifespan=lifespan, docs_url=f"{API}/docs",
                  openapi_url=f"{API}/openapi.json")
    app.state.service = service
    app.state.operations = operations
    app.state.alerts = alerts
    app.state.admin = admin
    app.state.history = history
    app.state.analytics = analytics
    app.state.dashboard = dashboard
    app.state.maintenance = maintenance
    app.state.access = access
    app.state.registry = registry
    app.state.hub = hub

    @app.exception_handler(ApiFail)
    async def _api_fail(_request, exc: ApiFail):
        error = {"code": exc.code, "message": exc.message} if exc.message else {"code": exc.code}
        return JSONResponse({"error": error}, status_code=exc.status)

    @app.exception_handler(RequestValidationError)
    async def _invalid(_request, _exc):
        return JSONResponse({"error": {"code": "request.invalid"}}, status_code=422)

    # Dependencies

    def session_db():
        with db.session() as s:
            yield s

    def current(request: Request, s: Session = Depends(session_db)) -> tuple:
        found = security.session_user(s, request.cookies.get(security.SESSION_COOKIE))
        if found is None:
            raise ApiFail(401, "auth.required")
        user = found[1]
        now = now_ms()
        if not user.last_active_at or now - user.last_active_at > ACTIVE_WRITE_MS:
            user.last_active_at = now
            s.commit()
        return found

    def writer(request: Request, auth=Depends(current)) -> tuple:
        session, _user = auth
        if request.headers.get(security.CSRF_HEADER) != session.csrf or not _same_origin(request.headers):
            raise ApiFail(403, "auth.csrf")
        return auth

    def require(user: User, permission: str):
        access.require(user, permission)

    def delivery_dto(s: Session, row: Delivery, viewer: User) -> dict:
        events = s.scalars(select(DeliveryEvent).where(DeliveryEvent.delivery_id == row.id)).all()
        return views.delivery(site, row, events, s.get(User, row.requester_id), viewer)

    def own_delivery(s: Session, user: User, delivery_id: int) -> Delivery:
        row = s.get(Delivery, delivery_id)
        if row is None or not access.task_visible(user, row):
            raise ApiFail(404, "delivery.not_found")
        return row

    def robot_visibility(user: User):
        return lambda robot: access.fleet_visible(user, robot["fleet"])

    def refresh_user(user_id: str) -> None:
        """Push the new access of an account to its open pages."""
        with db.session() as s:
            user = s.get(User, user_id)
        if user is not None:
            hub.notify_user(user.id, "access", security.can(user, "fleet.view"), robot_visibility(user))

    # Site

    @app.get(f"{API}/config")
    def config():
        sup = settings.data["support"]
        return {
            "siteName": {"vi": settings.get("site", "name"), "en": settings.get("site", "name")},
            "timeZone": settings.get("site", "time_zone"),
            "defaultLocale": settings.get("site", "default_locale"),
            "locales": list(LOCALES),
            "maxActiveDeliveries": settings.get("delivery", "max_open"),
            "minPasswordLength": settings.get("auth", "min_password_length"),
            "maxPatrolStops": settings.get("patrol", "max_stops"),
            "maxPatrolRounds": settings.get("patrol", "max_rounds"),
            "support": {"email": sup["email"], "phone": sup["phone"],
                        "hours": {"vi": sup["hours_vi"], "en": sup["hours_en"]}},
            "demo": False,
        }

    @app.get(f"{API}/status")
    def status():
        return {"rmf": service.rmf_status()}

    @app.get(f"{API}/levels")
    def levels(_auth=Depends(current)):
        return [{"id": l.id, "label": l.label, "imageUrl": f"{API}/levels/{l.id}/image?v={l.image_hash}",
                 "origin": list(l.origin), "resolution": l.resolution, "widthPx": l.width_px,
                 "heightPx": l.height_px} for l in site.levels.values()]

    @app.get(f"{API}/levels/{{level_id}}/graph")
    def level_graph(level_id: str, _auth=Depends(current)):
        level = site.levels.get(level_id)
        if level is None:
            raise ApiFail(404, "level.not_found")
        return {"levelId": level.id, "vertices": level.vertices, "lanes": level.lanes}

    @app.get(f"{API}/levels/{{level_id}}/image")
    def level_image(level_id: str, v: str = "", _auth=Depends(current)):
        level = site.levels.get(level_id)
        if level is None:
            raise ApiFail(404, "level.not_found")
        cache = "private, max-age=31536000, immutable" if v == level.image_hash else "private, no-cache"
        return Response(level.image, media_type=level.media_type,
                        headers={"Cache-Control": cache, "ETag": f'"{level.image_hash}"'})

    @app.get(f"{API}/locations")
    def locations(_auth=Depends(current)):
        return [views.location(l) for l in site.locations]

    @app.get(f"{API}/templates")
    def templates(auth=Depends(current)):
        allowed = set(access.services(auth[1]))
        out = []
        for tpl in site.templates:
            svc = registry.by_category(tpl["kind"])
            if svc is not None and svc.id in allowed:
                out.append(tpl | {"service": svc.id})
        return out

    @app.get(f"{API}/services")
    def services_list(auth=Depends(current)):
        return dashboard.services(auth[1])

    @app.get(f"{API}/catalog")
    def catalog(auth=Depends(current)):
        return dashboard.catalog(auth[1])

    # Session

    @app.post(f"{API}/auth/login")
    def login(request: Request, response: Response, body: dict = Body(...), s: Session = Depends(session_db)):
        email = str(body.get("email", "")).strip().lower()
        key = f"{email}|{request.client.host if request.client else ''}"
        if limiter.locked(key):
            raise ApiFail(429, "auth.too_many_attempts")
        user = user_by_email(s, email)
        if user is None or not user.active or not security.verify_password(user.password_hash, str(body.get("password", ""))):
            limiter.failed(key)
            db.audit(s, None, "auth.login_failed", email)
            s.commit()
            raise ApiFail(401, "auth.invalid_credentials")
        limiter.succeeded(key)
        security.purge_expired(s)
        session = security.create_session(s, user, settings.get("server", "session_hours"))
        user.last_login_at = now_ms()
        db.audit(s, user.id, "auth.login", user.email)
        s.commit()
        max_age = settings.get("server", "session_hours") * 3600
        response.set_cookie(security.SESSION_COOKIE, session.id, max_age=max_age, httponly=True,
                            samesite="lax", secure=secure_cookies, path="/")
        response.set_cookie(security.CSRF_COOKIE, session.csrf, max_age=max_age, httponly=False,
                            samesite="strict", secure=secure_cookies, path="/")
        return views.me(user, access)

    @app.post(f"{API}/auth/logout", status_code=204)
    def logout(request: Request, response: Response, s: Session = Depends(session_db)):
        found = security.session_user(s, request.cookies.get(security.SESSION_COOKIE))
        if found:
            s.delete(found[0])
            s.commit()
        response.delete_cookie(security.SESSION_COOKIE, path="/")
        response.delete_cookie(security.CSRF_COOKIE, path="/")
        response.status_code = 204
        return response

    @app.get(f"{API}/auth/me")
    def auth_me(auth=Depends(current)):
        return views.me(auth[1], access)

    @app.put(f"{API}/me/preferences")
    def preferences(body: dict = Body(...), auth=Depends(writer), s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        if "locale" in body:
            if body["locale"] not in LOCALES:
                raise ApiFail(422, "prefs.invalid_locale")
            user.locale = body["locale"]
        prefs = body.get("notificationPrefs")
        if isinstance(prefs, dict):
            merged = dict(user.prefs)
            for key in ("deliveryUpdates", "delays"):
                if isinstance(prefs.get(key), bool):
                    merged[key] = prefs[key]
            user.prefs = merged
        s.commit()
        return views.me(user, access)

    @app.post(f"{API}/me/password", status_code=204)
    def change_password(body: dict = Body(...), auth=Depends(writer), s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        if not security.verify_password(user.password_hash, str(body.get("currentPassword", ""))):
            raise ApiFail(422, "auth.wrong_password")
        new = str(body.get("newPassword", ""))
        if len(new) < settings.get("auth", "min_password_length"):
            raise ApiFail(422, "auth.password_too_short")
        user.password_hash = security.hash_password(new)
        db.audit(s, user.id, "auth.password_changed", user.email)
        s.commit()
        return Response(status_code=204)

    # Shortcuts

    @app.get(f"{API}/me/saved-locations")
    def saved_locations(auth=Depends(current), s: Session = Depends(session_db)):
        rows = s.scalars(select(SavedLocation).where(SavedLocation.user_id == auth[1].id)
                         .order_by(SavedLocation.position)).all()
        return [{"locationId": r.location_id, "starred": r.starred} for r in rows if site.location(r.location_id)]

    @app.put(f"{API}/me/saved-locations")
    def put_saved_locations(body: list = Body(...), auth=Depends(writer), s: Session = Depends(session_db)):
        items, seen = [], set()
        for item in body:
            loc_id = item.get("locationId") if isinstance(item, dict) else None
            if site.location(str(loc_id)) is None:
                raise ApiFail(422, "saved.unknown_location")
            if loc_id in seen:
                continue
            seen.add(loc_id)
            items.append({"locationId": loc_id, "starred": item.get("starred") is True})
        user_id = auth[1].id
        for row in s.scalars(select(SavedLocation).where(SavedLocation.user_id == user_id)).all():
            s.delete(row)
        s.flush()
        for i, item in enumerate(items):
            s.add(SavedLocation(user_id=user_id, location_id=item["locationId"], starred=item["starred"], position=i))
        s.commit()
        return items

    @app.get(f"{API}/me/recent-destinations")
    def recent_destinations(auth=Depends(current), s: Session = Depends(session_db)):
        rows = s.scalars(select(Delivery).where(Delivery.requester_id == auth[1].id, Delivery.status != "scheduled")
                         .order_by(Delivery.created_at.desc()).limit(50)).all()
        out, seen = [], set()
        for r in rows:
            if r.dropoff_id not in seen and site.location(r.dropoff_id):
                seen.add(r.dropoff_id)
                out.append({"locationId": r.dropoff_id, "lastUsedAt": r.created_at})
        return out[:4]

    # Deliveries and patrols

    @app.get(f"{API}/deliveries")
    def list_deliveries(group: str = "all", auth=Depends(current), s: Session = Depends(session_db)):
        user = auth[1]
        rows = s.scalars(select(Delivery).where(Delivery.requester_id == user.id)
                         .order_by(Delivery.created_at.desc()).limit(200)).all()
        items = [delivery_dto(s, r, user) for r in rows]
        rank = {"active": 0, "upcoming": 1, "completed": 2}
        items.sort(key=lambda d: (rank[d["group"]], (d["scheduledAt"] or 0) if d["group"] == "upcoming" else -d["createdAt"]))
        counts = {"all": len(items), "active": 0, "upcoming": 0, "completed": 0}
        for d in items:
            counts[d["group"]] += 1
        return {"items": items if group == "all" else [d for d in items if d["group"] == group],
                "counts": counts, "nextCursor": None}

    @app.get(f"{API}/deliveries/{{delivery_id}}")
    def get_delivery(delivery_id: int, auth=Depends(current), s: Session = Depends(session_db)):
        return delivery_dto(s, own_delivery(s, auth[1], delivery_id), auth[1])

    @app.post(f"{API}/deliveries", status_code=201)
    def create_delivery(body: dict = Body(...), auth=Depends(writer), s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        row = service.create(s, user, body)
        return delivery_dto(s, row, user)

    @app.post(f"{API}/deliveries/{{delivery_id}}/cancel")
    def cancel_delivery(delivery_id: int, auth=Depends(writer), s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        row = service.cancel(s, user, own_delivery(s, user, delivery_id))
        return delivery_dto(s, row, user)

    # Notifications

    @app.get(f"{API}/notifications")
    def notifications(auth=Depends(current), s: Session = Depends(session_db)):
        rows = s.scalars(select(Notification).where(Notification.user_id == auth[1].id)
                         .order_by(Notification.created_at.desc()).limit(200)).all()
        return {"items": [views.notification(r) for r in rows],
                "unread": sum(1 for r in rows if r.read_at is None), "nextCursor": None}

    @app.post(f"{API}/notifications/read-all", status_code=204)
    def read_all(auth=Depends(writer), s: Session = Depends(session_db)):
        s.execute(update(Notification).where(Notification.user_id == auth[1].id, Notification.read_at.is_(None))
                  .values(read_at=now_ms()))
        s.commit()
        return Response(status_code=204)

    @app.post(f"{API}/notifications/{{notification_id}}/read")
    def read_one(notification_id: int, auth=Depends(writer), s: Session = Depends(session_db)):
        row = s.get(Notification, notification_id)
        if row is None or row.user_id != auth[1].id:
            raise ApiFail(404, "notification.not_found")
        if row.read_at is None:
            row.read_at = now_ms()
            s.commit()
        return views.notification(row)

    # Operations overview and tasks of every service

    @app.get(f"{API}/overview")
    def overview(auth=Depends(current)):
        return dashboard.overview(auth[1])

    @app.get(f"{API}/tasks")
    def tasks_list(service_type: str = "", state: str = "", q: str = "", group: str = "all", mine: bool = False,
                   auth=Depends(current)):
        return dashboard.tasks(auth[1], service_type, state, q, group, mine)

    @app.post(f"{API}/tasks", status_code=201)
    def tasks_create(body: dict = Body(...), auth=Depends(writer), s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        row = service.create_task(s, user, body)
        return dashboard.task(user, row.id)

    @app.get(f"{API}/tasks/{{task_id}}")
    def tasks_get(task_id: int, auth=Depends(current)):
        return dashboard.task(auth[1], task_id)

    @app.post(f"{API}/tasks/{{task_id}}/{{action}}")
    def tasks_action(task_id: int, action: str, body: dict = Body(default={}), auth=Depends(writer),
                     s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        row = own_delivery(s, user, task_id)
        if action == "cancel":
            require(user, "task.cancel")
            service.cancel(s, user, row, operator=True)
        elif action in ("pause", "resume"):
            service.pause(s, user, row, resume=action == "resume")
        elif action == "reassign":
            row = service.reassign(s, user, row, str((body or {}).get("robot", "")))
        else:
            raise ApiFail(404, "task.unknown_action")
        return dashboard.task(user, row.id)

    @app.get(f"{API}/schedule")
    def schedule(start: int, end: int, service_type: str = "", robot: str = "", zone: str = "",
                 auth=Depends(current)):
        return dashboard.schedule(auth[1], start, end, service_type, robot, zone)

    @app.get(f"{API}/analytics/activity")
    def activity(range: str = "today", service_type: str = "", auth=Depends(current)):
        return dashboard.activity(auth[1], range, service_type)

    @app.get(f"{API}/analytics")
    def analytics_report(days: int = 7, service_type: str = "", robot: str = "", zone: str = "",
                         auth=Depends(current)):
        user = auth[1]
        require(user, "analytics.view")
        if service_type and service_type not in access.services(user):
            raise ApiFail(403, "SERVICE_NOT_ALLOWED")
        allowed = [svc for svc in registry.services if svc.id in access.services(user)]
        return analytics.report(days, lambda r: access.task_visible(user, r), service_type, robot, zone, allowed)

    @app.get(f"{API}/maintenance")
    def maintenance_table(auth=Depends(current)):
        user = auth[1]
        require(user, "maintenance.view")
        robots = operations.robots(user)
        return {"robots": maintenance.table(robots), "items": maintenance.items({r["name"] for r in robots})}

    @app.post(f"{API}/maintenance", status_code=201)
    def maintenance_create(body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "maintenance.manage")
        item = maintenance.create(auth[1], body, set(service._robots))
        maintenance.tick()
        return item

    @app.patch(f"{API}/maintenance/{{item_id}}")
    def maintenance_update(item_id: int, body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "maintenance.manage")
        item = maintenance.update(auth[1], item_id, body)
        maintenance.tick()
        return item

    @app.get(f"{API}/search")
    def search(q: str = "", auth=Depends(current), s: Session = Depends(session_db)):
        user = auth[1]
        needle = q.strip().lower()
        if len(needle) < 1:
            return {"robots": [], "tasks": [], "locations": [], "zones": [], "users": []}
        robots = [r for r in operations.robots(user) if needle in r["name"].lower() or needle in r["fleet"].lower()]
        found = dashboard.tasks(user, q=needle.lstrip("#"))["items"]
        texts = lambda item: [str(v).lower() for v in item["name"].values()]
        locations = [views.location(l) for l in site.locations if needle in l["id"] or any(needle in t for t in texts(l))]
        zones = [z for z in site.zones if needle in z["id"] or any(needle in t for t in texts(z))]
        users = []
        if security.can(user, "users.manage"):
            users = [{"id": u.id, "fullName": u.full_name, "email": u.email, "role": u.role}
                     for u in s.scalars(select(User)).all() if needle in u.full_name.lower() or needle in u.email]
        return {"robots": [{"name": r["name"], "fleet": r["fleet"], "serviceType": r["serviceType"], "status": r["status"]}
                           for r in robots[:SEARCH_LIMIT]],
                "tasks": [{"id": t["id"], "service": t["service"], "state": t["state"], "pickup": t["pickup"],
                           "dropoff": t["dropoff"], "area": t["area"], "route": t["route"]} for t in found[:SEARCH_LIMIT]],
                "locations": locations[:SEARCH_LIMIT], "zones": zones[:SEARCH_LIMIT], "users": users[:SEARCH_LIMIT]}

    # Fleet (permission fleet.view; robots of the viewer's services)

    @app.get(f"{API}/fleet/overview")
    def fleet_overview(auth=Depends(current)):
        require(auth[1], "fleet.view")
        return operations.overview()

    @app.get(f"{API}/fleet/tasks")
    def fleet_tasks(group: str = "active", kind: str = "", q: str = "", auth=Depends(current)):
        result = dashboard.tasks(auth[1], kind, "", q, group)
        return result | {"counts": result["groups"], "stateCounts": result["counts"]}

    @app.post(f"{API}/fleet/tasks/{{task_id}}/cancel")
    def fleet_cancel_task(task_id: int, auth=Depends(writer), s: Session = Depends(session_db)):
        user = s.get(User, auth[1].id)
        require(user, "task.cancel")
        service.cancel(s, user, own_delivery(s, user, task_id), operator=True)
        return dashboard.task(user, task_id)

    def alerts_route(state: str, auth):
        require(auth[1], "fleet.view")
        if state not in ("open", "all"):
            raise ApiFail(422, "alerts.invalid_state")
        return dashboard.visible_alerts(auth[1], state)

    @app.get(f"{API}/alerts")
    def alerts_list(state: str = "open", auth=Depends(current)):
        return alerts_route(state, auth)

    @app.get(f"{API}/fleet/alerts")
    def fleet_alerts(state: str = "open", auth=Depends(current)):
        return alerts_route(state, auth)

    def visible_alert_ids(user: User) -> set[int]:
        return {a["id"] for a in dashboard.visible_alerts(user, "all")["items"]}

    @app.post(f"{API}/fleet/alerts/ack-all")
    def fleet_alerts_ack_all(auth=Depends(writer)):
        user = auth[1]
        require(user, "alerts.ack")
        if access.sees_all(user):
            return {"acknowledged": alerts.acknowledge(user, None)}
        open_ids = [a["id"] for a in dashboard.visible_alerts(user)["items"] if a["ackedAt"] is None]
        return {"acknowledged": sum(alerts.acknowledge(user, i) for i in open_ids)}

    @app.post(f"{API}/fleet/alerts/{{alert_id}}/ack")
    def fleet_alert_ack(alert_id: int, auth=Depends(writer)):
        require(auth[1], "alerts.ack")
        if alert_id not in visible_alert_ids(auth[1]):
            raise ApiFail(404, "alert.not_found")
        return {"acknowledged": alerts.acknowledge(auth[1], alert_id)}

    @app.post(f"{API}/fleet/alerts/{{alert_id}}/resolve", status_code=204)
    def fleet_alert_resolve(alert_id: int, auth=Depends(writer)):
        require(auth[1], "alerts.ack")
        if alert_id not in visible_alert_ids(auth[1]):
            raise ApiFail(404, "alert.not_found")
        alerts.resolve(auth[1], alert_id)
        return Response(status_code=204)

    @app.get(f"{API}/fleet/robots")
    def fleet_robots(auth=Depends(current)):
        require(auth[1], "fleet.view")
        return operations.robots(auth[1])

    @app.get(f"{API}/fleet/robots/{{name}}")
    def fleet_robot(name: str, hours: float = 24.0, auth=Depends(current)):
        require(auth[1], "fleet.view")
        return admin.robot(name, hours, auth[1])

    @app.post(f"{API}/fleet/robots/{{robot}}/{{action}}")
    def fleet_robot_control(robot: str, action: str, body: dict = Body(default={}), auth=Depends(writer)):
        require(auth[1], "fleet.control")
        if not any(r["name"] == robot for r in operations.robots(auth[1])):
            raise ApiFail(404, "robot.not_found")
        return operations.control(auth[1], robot, action.replace("-", "_"), body or {})

    @app.get(f"{API}/fleet/registration")
    def fleet_registration(auth=Depends(current)):
        require(auth[1], "robots.manage")
        return operations.registration()

    @app.post(f"{API}/fleet/registration")
    def fleet_register(body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "robots.manage")
        return operations.register(auth[1], body)

    @app.get(f"{API}/fleet/lanes")
    def fleet_lanes(auth=Depends(current)):
        require(auth[1], "fleet.view")
        return operations.lanes()

    @app.post(f"{API}/fleet/lanes")
    def fleet_set_lanes(body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "locations.manage")
        return operations.set_lanes(auth[1], body)

    @app.get(f"{API}/fleet/nav-graph")
    def fleet_nav_graph(level: str | None = None, auth=Depends(current)):
        require(auth[1], "locations.manage")
        return operations.nav_graph(level)

    @app.put(f"{API}/fleet/nav-graph")
    def fleet_save_nav_graph(body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "locations.manage")
        return operations.save_nav_graph(auth[1], body)

    @app.get(f"{API}/fleet/system")
    def fleet_system(auth=Depends(current)):
        require(auth[1], "system.diagnostics")
        return operations.system()

    # Administration (admin role)

    @app.get(f"{API}/admin/overview")
    def admin_overview(auth=Depends(current)):
        require(auth[1], "system.diagnostics")
        return admin.overview()

    @app.get(f"{API}/admin/robots/{{name}}")
    def admin_robot(name: str, hours: float = 24.0, auth=Depends(current)):
        require(auth[1], "fleet.view")
        return admin.robot(name, hours, auth[1])

    @app.get(f"{API}/admin/analytics")
    def admin_analytics(days: int = 7, auth=Depends(current)):
        return analytics_report(days, auth=auth)

    @app.get(f"{API}/admin/infrastructure")
    def admin_infrastructure(auth=Depends(current)):
        require(auth[1], "locations.manage")
        return admin.infrastructure()

    @app.get(f"{API}/admin/integrations")
    def admin_integrations(auth=Depends(current)):
        require(auth[1], "integrations.manage")
        return dashboard.integrations()

    @app.get(f"{API}/admin/settings")
    def admin_settings(auth=Depends(current)):
        require(auth[1], "settings.manage")
        return admin.system_settings()

    @app.put(f"{API}/admin/toggles/{{kind}}/{{item_id}}")
    def admin_toggle(kind: str, item_id: str, body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "settings.manage")
        result = admin.set_toggle(auth[1], kind, item_id, body.get("enabled"))
        with db.session() as s:
            for uid in s.scalars(select(User.id)).all():
                hub.notify_user(uid, "access")
        return result

    @app.get(f"{API}/admin/audit")
    def admin_audit(q: str = "", action: str = "", limit: int = 200, auth=Depends(current)):
        require(auth[1], "system.diagnostics")
        return admin.audit(q, action, limit)

    @app.get(f"{API}/admin/access-catalog")
    def admin_access_catalog(auth=Depends(current)):
        require(auth[1], "users.manage")
        return admin.access_catalog()

    @app.get(f"{API}/admin/users")
    def admin_users(auth=Depends(current)):
        require(auth[1], "users.manage")
        return admin.users()

    @app.post(f"{API}/admin/users", status_code=201)
    def admin_create_user(body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "users.manage")
        return admin.create_user(auth[1], body)

    @app.patch(f"{API}/admin/users/{{user_id}}")
    def admin_update_user(user_id: str, body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "users.manage")
        result = admin.update_user(auth[1], user_id, body)
        refresh_user(user_id)
        return result

    @app.post(f"{API}/admin/users/{{user_id}}/password", status_code=204)
    def admin_set_password(user_id: str, body: dict = Body(...), auth=Depends(writer)):
        require(auth[1], "users.manage")
        admin.set_password(auth[1], user_id, body)
        return Response(status_code=204)

    @app.get(f"{API}/admin/roles")
    def admin_roles(auth=Depends(current)):
        require(auth[1], "users.manage")
        return admin.roles()

    # WebSockets

    @app.websocket("/ws")
    async def browser_socket(ws: WebSocket):
        if not _same_origin(ws.headers):
            await ws.close(code=4403)
            return
        with db.session() as s:
            found = security.session_user(s, ws.cookies.get(security.SESSION_COOKIE))
        if found is None:
            await ws.close(code=4401)
            return
        user = found[1]
        await ws.accept()
        conn = Connection(ws, user.id, security.can(user, "fleet.view"), robot_visibility(user))
        hub.add(conn)
        try:
            while True:
                await ws.receive_text()
        except WebSocketDisconnect:
            pass
        finally:
            hub.remove(conn)

    return app


def _same_origin(headers) -> bool:
    """Origin, when sent, must name the host the request was made to."""
    origin = headers.get("origin")
    if not origin:
        return True
    return urlparse(origin).netloc == headers.get("host", "")
