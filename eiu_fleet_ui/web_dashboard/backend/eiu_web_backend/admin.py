"""Administration: accounts and their access, the audit log, service and zone switches, the system overview and the
infrastructure of the nav graph."""

import math
import re
import time
import uuid

import yaml
from sqlalchemy import func, select

from . import security
from .db import Alert, AuditLog, Database, Delivery, DeliveryEvent, Toggle, User, now_ms, user_by_email
from .errors import ApiFail
from .settings import Settings
from .site import Site

EMAIL = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")
NAME_MAX = 200
AUDIT_RECENT = 8
ROBOT_TASKS = 10
ROBOT_EVENTS = 60
UTILIZATION_DAYS = 7


DEPARTMENT_MAX = 200


def user_dto(user: User) -> dict:
    admin = user.role == "admin"
    return {"id": user.id, "email": user.email, "fullName": user.full_name, "role": user.role,
            "department": user.department or "", "active": user.active, "locale": user.locale,
            "allowedServices": [] if admin else list(user.allowed_services or []),
            "allowedZones": [] if admin else list(user.allowed_zones or []),
            "permissions": security.permissions(user), "createdAt": user.created_at,
            "lastLoginAt": user.last_login_at, "lastActiveAt": user.last_active_at or user.last_login_at}


class Admin:
    def __init__(self, settings: Settings, site: Site, db: Database, operations, history, access=None):
        self.access = access
        self.settings = settings
        self.site = site
        self.db = db
        self.operations = operations
        self.history = history

    # Accounts

    def users(self) -> list[dict]:
        with self.db.session() as s:
            return [user_dto(u) for u in s.scalars(select(User).order_by(User.full_name)).all()]

    def _password(self, password) -> str:
        minimum = self.settings.get("auth", "min_password_length")
        if not isinstance(password, str) or len(password) < minimum:
            raise ApiFail(422, "auth.password_too_short")
        return security.hash_password(password)

    def _ids(self, value, known, code: str) -> list[str]:
        if not isinstance(value, list) or any(not isinstance(v, str) or v not in known for v in value):
            raise ApiFail(422, code)
        return list(dict.fromkeys(value))

    def _access_fields(self, body: dict) -> dict:
        """allowedServices, allowedZones and permissions of the body, checked against the catalog."""
        out = {}
        if "allowedServices" in body:
            out["allowed_services"] = self._ids(body["allowedServices"], {s.id for s in self.access.registry.services},
                                                "users.invalid_service")
        if "allowedZones" in body:
            out["allowed_zones"] = self._ids(body["allowedZones"], {z["id"] for z in self.site.zones}, "users.invalid_zone")
        if "permissions" in body:
            out["permissions"] = self._ids(body["permissions"], set(security.GRANTABLE), "users.invalid_permission")
        return out

    def create_user(self, actor: User, body: dict) -> dict:
        email = str(body.get("email", "")).strip().lower()
        name = str(body.get("fullName", "")).strip()
        department = str(body.get("department") or "").strip()
        role = body.get("role", "operator")
        if not EMAIL.match(email) or not name or len(name) > NAME_MAX or role not in security.ROLES \
                or len(department) > DEPARTMENT_MAX:
            raise ApiFail(422, "users.invalid")
        fields = self._access_fields(body)
        locale = body.get("locale") if body.get("locale") in ("vi", "en") else self.settings.get("site", "default_locale")
        with self.db.session() as s:
            if user_by_email(s, email):
                raise ApiFail(409, "users.email_taken")
            user = User(id=str(uuid.uuid4()), email=email, full_name=name, role=role, locale=locale,
                        department=department, active=body.get("active", True) is not False,
                        allowed_services=fields.get("allowed_services", []), allowed_zones=fields.get("allowed_zones", []),
                        permissions=fields.get("permissions", list(security.DEFAULT_OPERATOR)),
                        password_hash=self._password(body.get("password")))
            s.add(user)
            self.db.audit(s, actor.id, "user.create", email,
                          f"role={role}; services={','.join(user.allowed_services) or '-'}")
            s.commit()
            return user_dto(user)

    def update_user(self, actor: User, user_id: str, body: dict) -> dict:
        with self.db.session() as s:
            user = s.get(User, user_id)
            if user is None:
                raise ApiFail(404, "users.not_found")
            changes = []
            if "fullName" in body:
                name = str(body["fullName"]).strip()
                if not name or len(name) > NAME_MAX:
                    raise ApiFail(422, "users.invalid")
                if name != user.full_name:
                    changes.append(f"name: {user.full_name} → {name}")
                    user.full_name = name
            if "role" in body and body["role"] != user.role:
                if body["role"] not in security.ROLES:
                    raise ApiFail(422, "users.invalid")
                if user.id == actor.id:
                    raise ApiFail(409, "users.self")
                changes.append(f"role: {user.role} → {body['role']}")
                user.role = body["role"]
                security.end_sessions(s, user.id)
            if "department" in body:
                department = str(body["department"] or "").strip()
                if len(department) > DEPARTMENT_MAX:
                    raise ApiFail(422, "users.invalid")
                if department != (user.department or ""):
                    changes.append(f"department: {user.department or '-'} → {department or '-'}")
                    user.department = department
            for column, value in self._access_fields(body).items():
                if sorted(value) != sorted(getattr(user, column) or []):
                    label = column.replace("allowed_", "")
                    changes.append(f"{label}: {', '.join(value) or '-'}")
                    setattr(user, column, value)
            if "active" in body and bool(body["active"]) != user.active:
                if user.id == actor.id:
                    raise ApiFail(409, "users.self")
                user.active = bool(body["active"])
                changes.append("enabled" if user.active else "disabled")
                if not user.active:
                    security.end_sessions(s, user.id)
            if changes:
                self.db.audit(s, actor.id, "user.update", user.email, "; ".join(changes))
            s.commit()
            return user_dto(user)

    def set_password(self, actor: User, user_id: str, body: dict) -> None:
        with self.db.session() as s:
            user = s.get(User, user_id)
            if user is None:
                raise ApiFail(404, "users.not_found")
            user.password_hash = self._password(body.get("password"))
            security.end_sessions(s, user.id)
            self.db.audit(s, actor.id, "user.password", user.email, "")
            s.commit()

    @staticmethod
    def roles() -> list[dict]:
        return [{"role": "operator", "permissions": list(security.GRANTABLE)},
                {"role": "admin", "permissions": list(security.ALL_PERMISSIONS)}]

    def access_catalog(self) -> dict:
        """What the access editor offers: services, zones and the permissions an operator may be granted."""
        registry = self.access.registry
        return {"services": [{"id": s.id, "name": s.name, "icon": s.icon, "enabled": registry.enabled(s)}
                             for s in registry.services],
                "zones": [{"id": z["id"], "name": z["name"], "enabled": self.access.zone_enabled(z["id"])}
                          for z in self.site.zones],
                "permissionGroups": [{"id": g, "permissions": p} for g, p in security.OPERATOR_PERMISSIONS.items()],
                "adminPermissions": list(security.ADMIN_PERMISSIONS),
                "defaults": list(security.DEFAULT_OPERATOR)}

    # Service and zone switches

    def set_toggle(self, actor: User, kind: str, item_id: str, enabled) -> dict:
        known = {s.id for s in self.access.registry.services} if kind == "service" else {z["id"] for z in self.site.zones}
        if kind not in ("service", "zone") or item_id not in known or not isinstance(enabled, bool):
            raise ApiFail(422, "settings.invalid")
        with self.db.session() as s:
            row = s.get(Toggle, f"{kind}:{item_id}")
            if row is None:
                s.add(Toggle(key=f"{kind}:{item_id}", enabled=enabled))
            else:
                row.enabled = enabled
            self.db.audit(s, actor.id, f"{kind}.{'enable' if enabled else 'disable'}", item_id)
            s.commit()
        self.access.load_toggles()
        return {"kind": kind, "id": item_id, "enabled": enabled}

    def system_settings(self) -> dict:
        """Effective settings an admin reads on the settings page; secrets are left out."""
        data = {k: dict(v) for k, v in self.settings.data.items() if k not in ("redis", "server", "support")}
        data["server"] = {k: v for k, v in self.settings.data["server"].items() if k != "database"}
        registry = self.access.registry
        return {"settings": data,
                "services": [s.dto() | {"enabled": registry.enabled(s), "configured": s.enabled} for s in registry.services],
                "zones": [z | {"enabled": self.access.zone_enabled(z["id"]), "configured": z["enabled"]} for z in self.site.zones],
                "fleets": registry.fleets,
                "templates": self.site.templates,
                "sources": {"settings": str(self.settings.base_dir), "catalog": str(self.settings.path("site", "catalog")),
                            "services": str(self.settings.path("site", "services"))}}

    # Audit log

    def audit(self, query: str = "", action: str = "", limit: int = 200) -> dict:
        with self.db.session() as s:
            stmt = select(AuditLog).order_by(AuditLog.at.desc(), AuditLog.id.desc())
            if action:
                stmt = stmt.where(AuditLog.action.like(f"{action}%"))
            rows = s.scalars(stmt.limit(max(1, min(limit, 1000)) * (4 if query else 1))).all()
            names = {u.id: u.full_name for u in s.scalars(select(User)).all()}
            actions = sorted({a for (a,) in s.execute(select(AuditLog.action).distinct()).all()})
        needle = query.strip().lower()
        items = []
        for r in rows:
            item = {"id": r.id, "at": r.at, "actor": names.get(r.actor_id) if r.actor_id else None,
                    "action": r.action, "target": r.target, "detail": r.detail}
            if needle and not any(needle in str(v).lower() for v in item.values() if v):
                continue
            items.append(item)
        return {"items": items[:limit], "actions": actions}

    # Robot details

    def robot(self, name: str, hours: float = 24.0, user=None) -> dict:
        """Live state, technical status, recent tasks, battery trend, utilization and events of one robot."""
        ops = self.operations
        robot = next((r for r in ops.robots(user) if r["name"] == name), None)
        if robot is None:
            raise ApiFail(404, "robot.not_found")
        registry, fleet = {}, {}
        for entry in (ops.ops.get("registry") or {}).values():
            reg = entry.get("registry") or {}
            found = next((r for r in reg.get("robots", []) if r.get("name") == name), None)
            if found:
                registry, fleet = found, reg
        control = (ops.ops.get("controls") or {}).get(name) or {}
        received = (getattr(ops.rmf, "fleets_received", {}) if ops.rmf else {}).get(robot["fleet"])
        level = self.site.levels.get(robot["levelId"])
        nearest = None
        if level and level.vertices:
            v = min((v for v in level.vertices if v["name"]), default=None,
                    key=lambda v: math.hypot(v["x"] - robot["x"], v["y"] - robot["y"]))
            if v:
                nearest = {"name": v["name"], "distanceM": round(math.hypot(v["x"] - robot["x"], v["y"] - robot["y"]), 2)}
        raw = ops.service._robots.get(name, {})
        technical = {
            "rmf_mode": robot["mode"], "rmf_task": robot["taskId"], "nearest_waypoint": nearest["name"] if nearest else None,
            "nearest_distance_m": nearest["distanceM"] if nearest else None, "level": robot["levelId"],
            "position": f"x {robot['x']:.2f} · y {robot['y']:.2f} · {round(math.degrees(robot['yaw']))}°",
            "adapter": robot["adapter"], "controls": robot["controls"], "paused": robot["paused"],
            "speed_limit_mps": robot["speedLimit"], "fleet_state_age_s": round(time.time() - received / 1000, 1) if received else None,
            "interface": fleet.get("interface"), "manufacturer": registry.get("manufacturer"), "serial": registry.get("serial"),
            "charger": registry.get("charger"), "path_points": len(raw.get("path") or []),
        }
        with self.db.session() as s:
            rows = s.scalars(select(Delivery).where(Delivery.robot == name).order_by(Delivery.created_at.desc())
                             .limit(ROBOT_TASKS)).all()
            tasks = [{"id": r.id, "kind": r.kind, "status": r.status, "pickupId": r.pickup_id, "dropoffId": r.dropoff_id,
                      "createdAt": r.created_at, "finishedAt": r.finished_at} for r in rows]
            events = []
            for e in s.execute(select(DeliveryEvent, Delivery.id).join(Delivery, Delivery.id == DeliveryEvent.delivery_id)
                               .where(Delivery.robot == name).order_by(DeliveryEvent.at.desc()).limit(ROBOT_EVENTS)):
                ev = e[0]
                events.append({"at": ev.at, "source": "task", "level": "warning" if ev.type in ("failed", "delayed") else "info",
                               "code": ev.type, "deliveryId": e[1], "text": ev.detail})
            for a in s.scalars(select(Alert).where(Alert.robot == name).order_by(Alert.opened_at.desc()).limit(ROBOT_EVENTS)):
                events.append({"at": a.opened_at, "source": "alert", "level": a.severity, "code": a.code, "params": a.params or {},
                               "closedAt": a.resolved_at})
            names = {u.id: u.full_name for u in s.scalars(select(User)).all()}
            for a in s.scalars(select(AuditLog).where(AuditLog.target.in_([name, f"{robot['fleet']}/{name}"]))
                               .order_by(AuditLog.at.desc()).limit(ROBOT_EVENTS)):
                events.append({"at": a.at, "source": "audit", "level": "info", "code": a.action,
                               "actor": names.get(a.actor_id) if a.actor_id else None, "text": a.detail})
        events.sort(key=lambda e: -e["at"])
        return {"robot": robot, "technical": technical, "tasks": tasks, "events": events[:ROBOT_EVENTS],
                "battery": self.history.battery(name, hours), "hours": hours,
                "utilization": self.history.utilization(UTILIZATION_DAYS, name),
                "sampleS": self.settings.get("history", "sample_s")}

    # Overview

    def overview(self) -> dict:
        ops = self.operations
        base = ops.overview()
        registry = ops.ops.get("registry") or {}
        fleets = [f for f in base["health"] if f["key"] == "fleet"]
        integrations = [h for h in base["health"] if h["key"] in ("gateway", "rmf", "task_events", "adapter", "mqtt")]
        nav = ops.ops.get("nav_graph")
        with self.db.session() as s:
            users = s.scalar(select(func.count()).select_from(User).where(User.active.is_(True))) or 0
            admins = s.scalar(select(func.count()).select_from(User).where(User.active.is_(True), User.role == "admin")) or 0
            operators = s.scalar(select(func.count()).select_from(User).where(User.active.is_(True), User.role == "operator")) or 0
        recent = self.audit(limit=AUDIT_RECENT)["items"]
        return {
            **base,
            "fleets": {"configured": len(set(registry) | {f["params"]["name"] for f in fleets}),
                       "healthy": sum(1 for f in fleets if f["status"] == "ok")},
            "maps": {"levels": len(self.site.levels), "editable": bool(nav),
                     "navGraphPath": nav.get("path") if nav else None},
            "integrations": {"total": len(integrations), "healthy": sum(1 for h in integrations if h["status"] == "ok")},
            "users": {"active": users, "admins": admins, "operators": operators},
            "recentChanges": recent,
        }

    # Infrastructure

    def infrastructure(self) -> dict:
        """Chargers, doors and lifts of the nav graph the gateway serves, or of the site's own copy."""
        nav = self.operations.ops.get("nav_graph")
        try:
            doc = yaml.safe_load(nav["yaml"]) if nav else yaml.safe_load(self.settings.path("map", "nav_graph").read_text())
        except (yaml.YAMLError, OSError):
            doc = {}
        doc = doc or {}
        used_by = {}
        for entry in (self.operations.ops.get("registry") or {}).values():
            for charger in (entry.get("registry") or {}).get("chargers", []):
                used_by[charger.get("name")] = charger.get("used_by") or None
        chargers, doors = [], []
        for level_id, level in (doc.get("levels") or {}).items():
            for x, y, *rest in (level or {}).get("vertices", []):
                attrs = (rest[0] if rest else None) or {}
                if attrs.get("is_charger"):
                    chargers.append({"name": attrs.get("name", ""), "level": str(level_id), "x": float(x), "y": float(y),
                                     "usedBy": used_by.get(attrs.get("name"))})
            for name, door in ((level or {}).get("doors") or {}).items():
                doors.append({"name": str(name), "level": str(level_id), "type": (door or {}).get("door_type", "")})
        for name, door in (doc.get("doors") or {}).items():
            doors.append({"name": str(name), "level": str((door or {}).get("map", "")), "type": (door or {}).get("door_type", "")})
        lifts = [{"name": str(name), "levels": sorted(str(l) for l in ((lift or {}).get("levels") or {}))}
                 for name, lift in (doc.get("lifts") or {}).items()]
        return {"source": nav.get("path") if nav else str(self.settings.path("map", "nav_graph")),
                "chargers": chargers, "doors": doors, "lifts": lifts, "at": now_ms()}
