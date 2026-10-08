"""Authorization of every request: role, service access, zone access and action permissions.

The frontend hides what a user may not use; this module is what enforces it. An admin has every service, zone and
permission. An operator has the services, zones and permissions an admin granted; robots, tasks and alerts of other
services stay hidden unless the operator holds `fleet.view_all`.
"""

from sqlalchemy import select

from . import security
from .db import Database, Delivery, Toggle, User
from .errors import ApiFail
from .services import Registry, Service
from .site import Site

MESSAGES = {
    "PERMISSION_DENIED": {"en": "You do not have permission for this action.",
                          "vi": "Bạn không có quyền thực hiện thao tác này."},
    "ADMIN_ONLY": {"en": "Only an administrator can do this.", "vi": "Chỉ quản trị viên được thực hiện thao tác này."},
    "SERVICE_NOT_ALLOWED": {"en": "You do not have permission to create {service} tasks.",
                            "vi": "Bạn không có quyền tạo nhiệm vụ {service}."},
    "SERVICE_DISABLED": {"en": "The {service} service is disabled.", "vi": "Dịch vụ {service} đang tắt."},
    "ZONE_NOT_ALLOWED": {"en": "You do not have access to {zone}.", "vi": "Bạn không có quyền ở khu vực {zone}."},
    "ZONE_DISABLED": {"en": "{zone} is closed to robot tasks.", "vi": "Khu vực {zone} đang đóng với nhiệm vụ robot."},
    "NO_CAPABLE_FLEET": {"en": "No robot fleet can perform {service} tasks.",
                         "vi": "Chưa có đội robot nào thực hiện được nhiệm vụ {service}."},
    "ROBOT_NOT_CAPABLE": {"en": "{robot} cannot perform {service} tasks.",
                          "vi": "{robot} không thực hiện được nhiệm vụ {service}."},
}


def fail(user: User | None, status: int, code: str, **params) -> ApiFail:
    locale = user.locale if user is not None and user.locale in ("vi", "en") else "en"
    return ApiFail(status, code, MESSAGES[code][locale].format(**params))


class Access:
    def __init__(self, registry: Registry, site: Site, db: Database):
        self.registry = registry
        self.site = site
        self.db = db
        self.zone_overrides: dict[str, bool] = {}
        self.load_toggles()

    def load_toggles(self) -> None:
        with self.db.session() as s:
            rows = s.scalars(select(Toggle)).all()
        self.registry.overrides = {r.key[8:]: r.enabled for r in rows if r.key.startswith("service:")}
        self.zone_overrides = {r.key[5:]: r.enabled for r in rows if r.key.startswith("zone:")}

    # What the user has

    def services(self, user: User) -> list[str]:
        return security.allowed_services(user, self.registry.enabled_ids())

    def zones(self, user: User) -> set[str] | None:
        return security.allowed_zones(user)

    def zone_enabled(self, zone_id: str) -> bool:
        zone = self.site.zone(zone_id) or {}
        return self.zone_overrides.get(zone_id, zone.get("enabled", True))

    def require(self, user: User, permission: str) -> None:
        if not security.can(user, permission):
            code = "ADMIN_ONLY" if permission in security.ADMIN_PERMISSIONS else "PERMISSION_DENIED"
            raise fail(user, 403, code)

    def sees_all(self, user: User) -> bool:
        return user.role == "admin" or security.can(user, "fleet.view_all")

    # Visibility

    def fleet_visible(self, user: User, fleet: str) -> bool:
        if self.sees_all(user):
            return True
        return bool(set(self.registry.fleet_services(fleet)) & set(self.services(user)))

    def task_visible(self, user: User, row: Delivery) -> bool:
        if self.sees_all(user) or row.requester_id == user.id:
            return True
        if (row.service or row.kind) not in self.services(user):
            return False
        zones = self.zones(user)
        return zones is None or not row.zones or set(row.zones) <= zones

    def alert_visible(self, user: User, alert: dict, robot_fleets: dict[str, str], task_services: dict[int, str]) -> bool:
        """An alert about a robot or a task follows their visibility; an alert about the system is for admins and
        operators with `fleet.view_all`."""
        if self.sees_all(user):
            return True
        if alert.get("deliveryId") is not None:
            return task_services.get(alert["deliveryId"]) in self.services(user)
        if alert.get("robot"):
            fleet = robot_fleets.get(alert["robot"])
            return fleet is not None and self.fleet_visible(user, fleet)
        fleet = (alert.get("params") or {}).get("fleet")
        return bool(fleet) and self.fleet_visible(user, fleet)

    # Task creation

    def _service_name(self, user: User, service: Service) -> str:
        return service.name.get(user.locale if user.locale in ("vi", "en") else "en", service.id).lower()

    def check_service(self, user: User, service: Service | None) -> None:
        """Checks 2-5 of a new task (1 is the session): account active, role valid, service enabled and allowed."""
        if not user.active or user.role not in security.ROLES:
            raise fail(user, 403, "PERMISSION_DENIED")
        if service is None:
            raise ApiFail(422, "task.unknown_service")
        name = self._service_name(user, service)
        if not self.registry.enabled(service):
            raise fail(user, 409, "SERVICE_DISABLED", service=name)
        if service.id not in self.services(user):
            raise fail(user, 403, "SERVICE_NOT_ALLOWED", service=name)

    def check_scope(self, user: User, service: Service, zones: set[str], known_fleets) -> None:
        """Checks 6-8: the zones of the task are allowed and open, the user may create tasks, and a fleet is able
        to do the service."""
        allowed = self.zones(user)
        for zone_id in sorted(zones):
            label = (self.site.zone(zone_id) or {}).get("name", {}).get(user.locale, zone_id)
            if allowed is not None and zone_id not in allowed:
                raise fail(user, 403, "ZONE_NOT_ALLOWED", zone=label)
            if not self.zone_enabled(zone_id):
                raise fail(user, 409, "ZONE_DISABLED", zone=label)
        self.require(user, "task.create")
        if not self.registry.capable_fleets(service, known_fleets):
            raise fail(user, 409, "NO_CAPABLE_FLEET", service=self._service_name(user, service))

    def check_robot(self, user: User, service: Service, robot: str, fleet: str, permission: str = "fleet.assign") -> None:
        self.require(user, permission)
        if not self.fleet_visible(user, fleet) or not set(service.capabilities) <= set(self.registry.fleet_capabilities(fleet)):
            raise fail(user, 409, "ROBOT_NOT_CAPABLE", robot=robot, service=self._service_name(user, service))
