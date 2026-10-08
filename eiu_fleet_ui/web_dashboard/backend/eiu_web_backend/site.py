"""Campus catalog (floors, zones, locations, cleaning areas, patrol routes, templates) checked against the occupancy
map and the nav graph."""

import hashlib
import heapq
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from .settings import Settings

CATEGORIES = ("lab", "library", "center", "room", "cafeteria", "office")
TEMPLATE_ICONS = ("books", "lab", "food", "general", "qr", "patrol")


class SiteError(ValueError):
    pass


def image_info(data: bytes) -> tuple[str, int, int]:
    """Media type, width and height of a PNG or JPEG image."""
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        width, height = struct.unpack(">II", data[16:24])
        return "image/png", width, height
    if data[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            marker = data[i + 1]
            length = struct.unpack(">H", data[i + 2:i + 4])[0]
            if marker in (0xC0, 0xC1, 0xC2):
                height, width = struct.unpack(">HH", data[i + 5:i + 9])
                return "image/jpeg", width, height
            i += 2 + length
    raise SiteError("map image is neither PNG nor JPEG")


@dataclass
class Level:
    id: str
    label: dict
    origin: tuple[float, float]
    resolution: float
    image: bytes
    media_type: str
    width_px: int
    height_px: int
    vertices: list = field(default_factory=list)
    lanes: list = field(default_factory=list)

    @property
    def image_hash(self) -> str:
        return hashlib.sha256(self.image).hexdigest()[:12]

    def route(self, x: float, y: float, waypoint: str) -> list[tuple[float, float]] | None:
        """Shortest lane route from a point (entering at its nearest vertex) to a waypoint."""
        names = [v["name"] for v in self.vertices]
        if waypoint not in names or not self.vertices:
            return None
        goal = names.index(waypoint)
        start = min(range(len(self.vertices)), key=lambda i: math.hypot(self.vertices[i]["x"] - x, self.vertices[i]["y"] - y))
        adjacent: dict[int, list[int]] = {}
        for a, b in self.lanes:
            adjacent.setdefault(a, []).append(b)
        cost = {start: 0.0}
        previous: dict[int, int] = {}
        queue = [(0.0, start)]
        while queue:
            c, u = heapq.heappop(queue)
            if u == goal:
                break
            if c > cost.get(u, math.inf):
                continue
            for v in adjacent.get(u, []):
                step = math.hypot(self.vertices[v]["x"] - self.vertices[u]["x"], self.vertices[v]["y"] - self.vertices[u]["y"])
                if c + step < cost.get(v, math.inf):
                    cost[v] = c + step
                    previous[v] = u
                    heapq.heappush(queue, (c + step, v))
        if goal not in cost:
            return None
        chain = [goal]
        while chain[-1] != start:
            chain.append(previous[chain[-1]])
        return [(self.vertices[i]["x"], self.vertices[i]["y"]) for i in reversed(chain)]


def _text(value, where: str) -> dict:
    if not isinstance(value, dict) or not all(isinstance(value.get(k), str) and value.get(k) for k in ("vi", "en")):
        raise SiteError(f"{where}: needs vi and en text")
    return {"vi": value["vi"], "en": value["en"]}


def _read_graph(path: Path) -> dict:
    levels = (yaml.safe_load(path.read_text()) or {}).get("levels") or {}
    graphs = {}
    for level_id, level in levels.items():
        vertices = []
        for x, y, *rest in level.get("vertices", []):
            attrs = (rest[0] if rest else None) or {}
            vertices.append({"name": attrs.get("name", ""), "x": float(x), "y": float(y),
                             "charger": bool(attrs.get("is_charger", False))})
        lanes = [[int(a), int(b)] for a, b, *_ in level.get("lanes", [])]
        graphs[str(level_id)] = {"vertices": vertices, "lanes": lanes}
    if not graphs:
        raise SiteError(f"{path}: no levels")
    return graphs


class Site:
    def __init__(self, levels: dict[str, Level], locations: list[dict], templates: list[dict], rmf_levels: dict,
                 zones: list[dict] = (), areas: list[dict] = (), routes: list[dict] = ()):
        self.levels = levels
        self.locations = locations
        self.templates = templates
        self.rmf_levels = rmf_levels
        self.zones = list(zones)
        self.areas = list(areas)
        self.routes = list(routes)
        self._by_id = {loc["id"]: loc for loc in locations}
        self._zones = {z["id"]: z for z in self.zones}
        self._areas = {a["id"]: a for a in self.areas}
        self._routes = {r["id"]: r for r in self.routes}
        # RMF numbers lanes across the whole nav graph, level after level in file order.
        self.lane_offsets = {level_id: 0 for level_id in levels}
        self.lane_total = sum(len(level.lanes) for level in levels.values())

    def set_lane_layout(self, graphs: dict) -> None:
        """Lane offsets of every level from the level graphs, in nav graph file order."""
        offset, offsets = 0, {}
        for level_id, graph in graphs.items():
            offsets[level_id] = offset
            offset += len(graph["lanes"])
        self.lane_offsets = {level_id: offsets.get(level_id, 0) for level_id in self.levels}
        self.lane_total = offset

    def location(self, location_id: str) -> dict | None:
        return self._by_id.get(location_id)

    def zone(self, zone_id: str) -> dict | None:
        return self._zones.get(zone_id)

    def area(self, area_id: str) -> dict | None:
        return self._areas.get(area_id)

    def route_of(self, route_id: str) -> dict | None:
        return self._routes.get(route_id)

    def nearest_location(self, level_id: str, x: float, y: float) -> dict | None:
        on_level = [l for l in self.locations if l["levelId"] == level_id]
        return min(on_level, key=lambda l: math.hypot(l["x"] - x, l["y"] - y), default=None)

    def update_graph(self, level_id: str, vertices: list[dict], lanes: list) -> None:
        """Replace a level's nav graph (after it was edited) and move the catalog locations with their waypoints."""
        level = self.levels.get(level_id)
        if level is None:
            return
        level.vertices = vertices
        level.lanes = lanes
        by_name = {v["name"]: v for v in vertices if v["name"]}
        for loc in self.locations:
            vertex = by_name.get(loc["waypoint"]) if loc["levelId"] == level_id else None
            if vertex is not None:
                loc["x"], loc["y"] = vertex["x"], vertex["y"]

    def level_of_rmf(self, rmf_level: str) -> str:
        """Site level of an RMF level name; the only level when there is one."""
        if rmf_level in self.rmf_levels:
            return self.rmf_levels[rmf_level]
        if rmf_level in self.levels:
            return rmf_level
        return next(iter(self.levels)) if len(self.levels) == 1 else rmf_level

    @classmethod
    def load(cls, settings: Settings) -> "Site":
        graphs = _read_graph(settings.path("map", "nav_graph"))
        map_yaml = settings.path("map", "map_yaml")
        meta = yaml.safe_load(map_yaml.read_text())
        image = (map_yaml.parent / meta["image"]).read_bytes()
        media_type, width, height = image_info(image)
        level_id = settings.get("map", "level") or (next(iter(graphs)) if len(graphs) == 1 else "")
        if level_id not in graphs:
            raise SiteError(f"map.level {level_id!r} is not a level of the nav graph {sorted(graphs)}")

        catalog_path = settings.path("site", "catalog")
        catalog = yaml.safe_load(catalog_path.read_text()) or {}
        labels = catalog.get("levels") or {}
        origin = meta["origin"]
        level = Level(
            id=level_id,
            label=_text(labels.get(level_id, {"vi": level_id, "en": level_id}), f"levels.{level_id}"),
            origin=(float(origin[0]), float(origin[1])),
            resolution=float(meta["resolution"]),
            image=image, media_type=media_type, width_px=width, height_px=height,
            vertices=graphs[level_id]["vertices"], lanes=graphs[level_id]["lanes"],
        )
        levels = {level_id: level}

        zones = []
        for i, raw in enumerate(catalog.get("zones") or []):
            where = f"zones[{i}]"
            zone_id = str(raw.get("id", ""))
            if not zone_id or any(z["id"] == zone_id for z in zones):
                raise SiteError(f"{where}: missing or repeated id {zone_id!r}")
            if str(raw.get("level", "")) not in levels:
                raise SiteError(f"{where}: level {raw.get('level')!r} has no map")
            zones.append({"id": zone_id, "name": _text(raw.get("name"), f"{where}.name"), "levelId": str(raw["level"]),
                          "enabled": bool(raw.get("enabled", True))})
        zone_ids = {z["id"] for z in zones}

        locations = []
        for i, raw in enumerate(catalog.get("locations") or []):
            where = f"locations[{i}]"
            loc_id = str(raw.get("id", ""))
            if not loc_id or any(l["id"] == loc_id for l in locations):
                raise SiteError(f"{where}: missing or repeated id {loc_id!r}")
            if raw.get("category") not in CATEGORIES:
                raise SiteError(f"{where}: category must be one of {CATEGORIES}")
            lvl = levels.get(str(raw.get("level", "")))
            if lvl is None:
                raise SiteError(f"{where}: level {raw.get('level')!r} has no map")
            vertex = next((v for v in lvl.vertices if v["name"] == raw.get("waypoint")), None)
            if vertex is None:
                raise SiteError(f"{where}: waypoint {raw.get('waypoint')!r} is not in the nav graph")
            if raw.get("zone") not in zone_ids:
                raise SiteError(f"{where}: zone {raw.get('zone')!r} is not in zones")
            locations.append({
                "id": loc_id,
                "name": _text(raw.get("name"), f"{where}.name"),
                "building": _text(raw.get("building"), f"{where}.building"),
                "category": raw["category"],
                "zone": raw["zone"],
                "levelId": lvl.id,
                "waypoint": vertex["name"],
                "x": vertex["x"],
                "y": vertex["y"],
                "dispenser": raw.get("dispenser") or settings.get("delivery", "pickup_handler"),
                "ingestor": raw.get("ingestor") or settings.get("delivery", "dropoff_handler"),
            })
        known = {l["id"] for l in locations}

        areas = []
        for i, raw in enumerate(catalog.get("areas") or []):
            where = f"areas[{i}]"
            area_id = str(raw.get("id", ""))
            if not area_id or any(a["id"] == area_id for a in areas) or raw.get("zone") not in zone_ids:
                raise SiteError(f"{where}: missing or repeated id, or unknown zone")
            if not raw.get("rmf_zone"):
                raise SiteError(f"{where}: rmf_zone is required")
            areas.append({"id": area_id, "name": _text(raw.get("name"), f"{where}.name"), "zone": raw["zone"],
                          "rmfZone": str(raw["rmf_zone"])})

        routes = []
        for i, raw in enumerate(catalog.get("routes") or []):
            where = f"routes[{i}]"
            route_id = str(raw.get("id", ""))
            stops = [str(x) for x in raw.get("stops") or []]
            if not route_id or any(r["id"] == route_id for r in routes) or raw.get("zone") not in zone_ids:
                raise SiteError(f"{where}: missing or repeated id, or unknown zone")
            if not stops or any(x not in known for x in stops):
                raise SiteError(f"{where}: stops must be known locations")
            routes.append({"id": route_id, "name": _text(raw.get("name"), f"{where}.name"), "zone": raw["zone"],
                           "stops": stops})
        route_ids = {r["id"] for r in routes}

        templates = []
        for i, raw in enumerate(catalog.get("templates") or []):
            where = f"templates[{i}]"
            kind = raw.get("kind", "delivery")
            if kind not in ("delivery", "patrol") or raw.get("icon") not in TEMPLATE_ICONS:
                raise SiteError(f"{where}: bad kind or icon")
            stops = [str(s) for s in raw.get("stops") or []]
            for ref in [raw.get("pickup"), raw.get("dropoff"), *stops]:
                if ref is not None and ref not in known:
                    raise SiteError(f"{where}: unknown location {ref!r}")
            package_type = str(raw.get("package_type", "general"))
            if raw.get("route") is not None and raw.get("route") not in route_ids:
                raise SiteError(f"{where}: unknown route {raw.get('route')!r}")
            templates.append({
                "id": str(raw.get("id", f"t-{i}")),
                "name": _text(raw.get("name"), f"{where}.name"),
                "subtitle": _text(raw.get("subtitle"), f"{where}.subtitle"),
                "icon": raw["icon"],
                "kind": kind,
                "pickupId": raw.get("pickup"),
                "dropoffId": raw.get("dropoff"),
                "stops": stops,
                "routeId": raw.get("route"),
                "rounds": int(raw.get("rounds", 1)),
                "packageType": package_type,
                "available": bool(raw.get("available", True)),
            })
        site = cls(levels, locations, templates, dict(settings.get("map", "rmf_levels")), zones, areas, routes)
        site.set_lane_layout(graphs)
        return site
