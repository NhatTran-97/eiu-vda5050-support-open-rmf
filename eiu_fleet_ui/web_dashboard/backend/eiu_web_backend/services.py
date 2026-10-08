"""Robot services (config/services.yaml): the task form of each service, its RMF task category and the fleets able
to do it. Task parameters are validated against the service's form, so a new service with an existing category needs
only configuration."""

from dataclasses import dataclass, field

import yaml

from .site import Site

CATEGORIES = ("delivery", "patrol", "clean")
FIELD_TYPES = ("location", "locations", "area", "route", "zone", "select", "number", "text", "schedule")
ICONS = ("package", "sparkles", "shield", "bot", "truck", "camera", "wrench")
REPEATS = ("none", "daily", "weekdays", "weekly")
# Fields the category's request builder reads (rmf/requests.py).
CATEGORY_FIELDS = {"delivery": {"pickup", "dropoff"}, "patrol": set(), "clean": {"area"}}


class ServiceError(ValueError):
    pass


class FormError(Exception):
    def __init__(self, code: str, key: str = ""):
        super().__init__(code)
        self.code = code
        self.key = key


def _text(value, where: str) -> dict:
    if not isinstance(value, dict) or not all(isinstance(value.get(k), str) and value.get(k) for k in ("vi", "en")):
        raise ServiceError(f"{where}: needs vi and en text")
    return {"vi": value["vi"], "en": value["en"]}


@dataclass
class Service:
    id: str
    name: dict
    description: dict
    icon: str
    enabled: bool
    category: str
    capabilities: list[str]
    form: list[dict] = field(default_factory=list)

    def field(self, key: str) -> dict | None:
        return next((f for f in self.form if f["key"] == key), None)

    @property
    def schedule(self) -> dict | None:
        return next((f for f in self.form if f["type"] == "schedule"), None)

    def dto(self) -> dict:
        return {"id": self.id, "name": self.name, "description": self.description, "icon": self.icon,
                "enabled": self.enabled, "category": self.category, "requiredCapabilities": self.capabilities,
                "taskFormSchema": [dict(f) for f in self.form]}


def _field(raw: dict, where: str, keys: set[str]) -> dict:
    key, kind = str(raw.get("key", "")), raw.get("type")
    if not key or key in keys:
        raise ServiceError(f"{where}: missing or repeated key {key!r}")
    if kind not in FIELD_TYPES:
        raise ServiceError(f"{where}: type must be one of {FIELD_TYPES}")
    out = {"key": key, "type": kind, "required": bool(raw.get("required", False))}
    if kind == "select":
        options = [str(o) for o in raw.get("options") or []]
        if not options:
            raise ServiceError(f"{where}: a select needs options")
        out["options"] = options
        out["default"] = str(raw.get("default", options[0]))
        if out["default"] not in options:
            raise ServiceError(f"{where}: default is not an option")
    if kind == "number":
        out["min"] = float(raw.get("min", 0))
        out["max"] = float(raw.get("max", 1e9))
        if raw.get("default") is not None:
            out["default"] = float(raw["default"])
        if raw.get("unit"):
            out["unit"] = str(raw["unit"])
    if kind == "text":
        out["max"] = int(raw.get("max", 200))
    if kind == "locations":
        out["maxItems"] = int(raw.get("max_items", 8))
    if kind == "schedule":
        out["repeat"] = bool(raw.get("repeat", False))
    if raw.get("differs_from"):
        out["differsFrom"] = str(raw["differs_from"])
    if raw.get("filter"):
        out["filter"] = str(raw["filter"])
    if raw.get("label") is not None:
        out["label"] = _text(raw["label"], f"{where}.label")
    return out


class Registry:
    def __init__(self, services: list[Service], fleets: dict[str, dict]):
        self.services = services
        self.fleets = fleets
        self._by_id = {s.id: s for s in services}
        # Admin overrides of `enabled` (settings page), kept in the database.
        self.overrides: dict[str, bool] = {}

    @classmethod
    def load(cls, path, site: Site | None = None) -> "Registry":
        doc = yaml.safe_load(path.read_text()) or {}
        services = []
        for i, raw in enumerate(doc.get("services") or []):
            where = f"services[{i}]"
            sid = str(raw.get("id", ""))
            if not sid or any(s.id == sid for s in services):
                raise ServiceError(f"{where}: missing or repeated id {sid!r}")
            if raw.get("category") not in CATEGORIES:
                raise ServiceError(f"{where}: category must be one of {CATEGORIES}")
            if raw.get("icon", "bot") not in ICONS:
                raise ServiceError(f"{where}: icon must be one of {ICONS}")
            keys: set[str] = set()
            form = []
            for j, f in enumerate(raw.get("form") or []):
                form.append(_field(f or {}, f"{where}.form[{j}]", keys))
                keys.add(form[-1]["key"])
            missing = CATEGORY_FIELDS[raw["category"]] - keys
            if missing:
                raise ServiceError(f"{where}: category {raw['category']} needs the fields {sorted(missing)}")
            if raw["category"] == "patrol" and not ({"route", "stops"} & keys):
                raise ServiceError(f"{where}: a patrol needs a route or a stops field")
            for f in form:
                if f.get("differsFrom") and f["differsFrom"] not in keys:
                    raise ServiceError(f"{where}: {f['key']}.differs_from names no field")
                if f.get("filter") and f["filter"] not in keys:
                    raise ServiceError(f"{where}: {f['key']}.filter names no field")
            caps = [str(c) for c in raw.get("capabilities") or []]
            if not caps:
                raise ServiceError(f"{where}: capabilities are required")
            services.append(Service(sid, _text(raw.get("name"), f"{where}.name"),
                                    _text(raw.get("description"), f"{where}.description"), raw.get("icon", "bot"),
                                    bool(raw.get("enabled", True)), raw["category"], caps, form))
        fleets = {}
        for name, raw in (doc.get("fleets") or {}).items():
            raw = raw or {}
            if raw.get("service") is not None and raw["service"] not in {s.id for s in services}:
                raise ServiceError(f"fleets.{name}: unknown service {raw['service']!r}")
            fleets[str(name)] = {"service": raw.get("service"), "capabilities": [str(c) for c in raw.get("capabilities") or []]}
        return cls(services, fleets)

    def get(self, service_id: str) -> Service | None:
        return self._by_id.get(service_id)

    def enabled(self, service: Service) -> bool:
        return self.overrides.get(service.id, service.enabled)

    def enabled_ids(self) -> list[str]:
        return [s.id for s in self.services if self.enabled(s)]

    def by_category(self, category: str) -> Service | None:
        return next((s for s in self.services if s.category == category), None)

    # Fleets and robots

    def fleet_capabilities(self, fleet: str) -> list[str]:
        return list((self.fleets.get(fleet) or {}).get("capabilities") or [])

    def fleet_services(self, fleet: str) -> list[str]:
        """Services whose required capabilities the fleet's robots have, in configuration order."""
        caps = set(self.fleet_capabilities(fleet))
        return [s.id for s in self.services if caps and set(s.capabilities) <= caps]

    def fleet_service(self, fleet: str) -> str | None:
        """Primary service of a fleet (its robot type), or None for a fleet the configuration does not know."""
        primary = (self.fleets.get(fleet) or {}).get("service")
        if primary:
            return primary
        services = self.fleet_services(fleet)
        return services[0] if services else None

    def capable_fleets(self, service: Service, known_fleets) -> list[str]:
        """Fleets, among the configured and the reporting ones, whose robots have the service's capabilities."""
        names = set(self.fleets) | set(known_fleets)
        return sorted(f for f in names if set(service.capabilities) <= set(self.fleet_capabilities(f)))

    # Task forms

    def validate(self, service: Service, params: dict, site: Site) -> tuple[dict, set[str]]:
        """Parameters checked against the service's form, with defaults filled in, and the zones they touch."""
        if not isinstance(params, dict):
            raise FormError("task.invalid_parameters")
        out, zones = {}, set()
        for f in service.form:
            key, kind = f["key"], f["type"]
            if kind == "schedule":
                continue
            value = params.get(key)
            if value in (None, "", []):
                if f.get("default") is not None:
                    out[key] = f["default"]
                elif f["required"]:
                    raise FormError("task.field_required", key)
                continue
            if kind == "location":
                loc = site.location(str(value))
                if loc is None:
                    raise FormError("task.unknown_location", key)
                out[key] = loc["id"]
                zones.add(loc["zone"])
            elif kind == "locations":
                if not isinstance(value, list) or len(value) > f["maxItems"]:
                    raise FormError("task.invalid_stops", key)
                locs = [site.location(str(v)) for v in value]
                if any(l is None for l in locs):
                    raise FormError("task.unknown_location", key)
                if any(a["id"] == b["id"] for a, b in zip(locs, locs[1:])):
                    raise FormError("task.repeated_stop", key)
                out[key] = [l["id"] for l in locs]
                zones.update(l["zone"] for l in locs)
            elif kind == "area":
                area = site.area(str(value))
                if area is None:
                    raise FormError("task.unknown_area", key)
                out[key] = area["id"]
                zones.add(area["zone"])
            elif kind == "route":
                route = site.route_of(str(value))
                if route is None:
                    raise FormError("task.unknown_route", key)
                out[key] = route["id"]
                zones.add(route["zone"])
            elif kind == "zone":
                if site.zone(str(value)) is None:
                    raise FormError("task.unknown_zone", key)
                out[key] = str(value)
                zones.add(str(value))
            elif kind == "select":
                if str(value) not in f["options"]:
                    raise FormError("task.invalid_option", key)
                out[key] = str(value)
            elif kind == "number":
                if not isinstance(value, (int, float)) or isinstance(value, bool) or not f["min"] <= value <= f["max"]:
                    raise FormError("task.invalid_number", key)
                out[key] = value
            elif kind == "text":
                text = str(value).strip()
                if len(text) > f["max"]:
                    raise FormError("task.text_too_long", key)
                out[key] = text
        for f in service.form:
            other = f.get("differsFrom")
            if other and out.get(f["key"]) is not None and out.get(f["key"]) == out.get(other):
                raise FormError("task.same_location", f["key"])
            parent = f.get("filter")
            if parent and out.get(f["key"]) and out.get(parent):
                item = site.route_of(out[f["key"]]) if f["type"] == "route" else site.area(out[f["key"]])
                if item and item["zone"] != out[parent]:
                    raise FormError("task.not_in_zone", f["key"])
        return out, zones
