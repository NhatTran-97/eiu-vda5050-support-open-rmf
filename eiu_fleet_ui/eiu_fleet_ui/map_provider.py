"""Expose the occupancy map and adapter navigation graph to QML."""

import json
import os
from pathlib import Path

import yaml
from PySide6.QtCore import QObject, Signal, Property, QUrl
from PySide6.QtGui import QImage

from .config import FleetConfig


def _maps_dir() -> Path:
    """The package's maps/ directory, from source or an installed share/."""
    override = os.environ.get("EIU_MAP_DIR")
    if override:
        return Path(override)

    source_dir = Path(__file__).resolve().parent.parent / "maps"
    if source_dir.is_dir():
        return source_dir

    from ament_index_python.packages import get_package_share_directory
    return Path(get_package_share_directory("eiu_fleet_ui")) / "maps"


class MapProvider(QObject):
    """Load the map image and navigation graph for QML."""

    mapReady = Signal()   # Emitted when the map is ready

    def __init__(self, config: FleetConfig | None = None, parent=None):
        super().__init__(parent)

        self._image_path = ""
        self._origin_x = 0.0
        self._origin_y = 0.0
        self._resolution = 0.05
        self._px_w = 0
        self._px_h = 0
        self._waypoints = []   # Waypoint records
        self._lanes = []       # Deduplicated lane endpoints
        # Map raw lane indices to deduplicated lanes for drawing.
        self._raw_lane_to_edge = []

        maps = _maps_dir()
        self._map_yaml = maps / "map.yaml"
        self._map_png = maps / "map.png"

        # Prefer the adapter's navigation graph.
        adapter_graph = config.nav_graph if config else None
        self._nav_graph = adapter_graph or (maps / "nav_graph.yaml")

        self._load()

    # Load the map image and navigation graph.

    def _load(self):
        """Read map.yaml, measure the map image, then read the nav graph."""
        self._read_map_yaml()
        self._load_png()
        self._read_nav_graph()
        print(f"[MAP] graph {self._nav_graph} — {len(self._waypoints)} waypoints, "
              f"{len(self._lanes)} lanes")
        self.mapReady.emit()

    def _read_map_yaml(self):
        with open(self._map_yaml) as f:
            data = yaml.safe_load(f)
        origin = data["origin"]          # Map origin pose
        self._origin_x = float(origin[0])
        self._origin_y = float(origin[1])
        self._resolution = float(data["resolution"])

    def _load_png(self):
        """Read image dimensions for map coordinate conversion."""
        img = QImage(str(self._map_png))
        self._px_w = img.width()
        self._px_h = img.height()
        self._image_path = str(self._map_png)

    def _read_nav_graph(self):
        """Read waypoints and lanes from the nav graph."""
        with open(self._nav_graph) as f:
            data = yaml.safe_load(f)
        levels = data["levels"]
        level = next(iter(levels.values()))   # Display the first level
        vertices = level["vertices"]

        self._waypoints = []
        for v in vertices:
            props = v[2]
            self._waypoints.append({
                "name":    props.get("name", ""),
                "x":       float(v[0]),
                "y":       float(v[1]),
                "charger": bool(props.get("is_charger", False)),
                "parking": bool(props.get("is_parking_spot", False)),
            })

        # RMF's graph (and close_lanes/lane_states) index each direction of a
        # lane separately, but the UI draws one line per corridor -- "raw"
        # keeps the 1-2 real graph indices behind each deduplicated lane so
        # closing/highlighting an edge can act on all of them.
        lanes_raw = level.get("lanes", [])
        all_pairs = {(int(ln[0]), int(ln[1])) for ln in lanes_raw}
        seen: dict[tuple[int, int], int] = {}
        lanes, raw_to_edge = [], []
        for raw_idx, ln in enumerate(lanes_raw):
            a, b = int(ln[0]), int(ln[1])
            key = (min(a, b), max(a, b))
            if key not in seen:
                seen[key] = len(lanes)
                lanes.append({"from": a, "to": b,
                              "bidir": (b, a) in all_pairs, "raw": []})
            lanes[seen[key]]["raw"].append(raw_idx)
            raw_to_edge.append(seen[key])
        self._lanes = lanes
        self._raw_lane_to_edge = raw_to_edge

    def waypoints(self) -> list:
        """Waypoints read from the nav graph, as {name, x, y, charger, parking}."""
        return self._waypoints

    # Map properties exposed to QML.

    @Property(str, notify=mapReady)
    def imagePath(self):
        return (QUrl.fromLocalFile(self._image_path).toString()
                if self._image_path else "")

    @Property(float, notify=mapReady)
    def originX(self): return self._origin_x

    @Property(float, notify=mapReady)
    def originY(self): return self._origin_y

    @Property(float, notify=mapReady)
    def resolution(self): return self._resolution

    @Property(int, notify=mapReady)
    def pixelW(self): return self._px_w

    @Property(int, notify=mapReady)
    def pixelH(self): return self._px_h

    @Property(str, notify=mapReady)
    def wpJson(self):
        return json.dumps(self._waypoints)

    @Property(str, notify=mapReady)
    def lanesJson(self):
        return json.dumps(self._lanes)

    @Property(str, notify=mapReady)
    def laneIndexMapJson(self):
        return json.dumps(self._raw_lane_to_edge)
