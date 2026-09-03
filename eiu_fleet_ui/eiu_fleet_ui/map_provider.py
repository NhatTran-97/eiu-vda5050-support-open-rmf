"""Occupancy grid and navigation graph, exposed to QML.

The nav graph is read from wherever the adapter reads it, so the UI cannot show
waypoints RMF does not have. The package's own copy under maps/ is only the
fallback for running the UI without the adapter present.
"""

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
    """
    Loads the occupancy-grid map (PGM/PNG) and nav_graph, and exposes them to QML.

    QML usage:
        mapProv.imagePath   -> "file:///path/to/map.png"
        mapProv.originX/Y   -> map origin coordinates (metres)
        mapProv.resolution  -> metres per pixel
        mapProv.pixelW/H    -> image size (pixels)
        mapProv.wpJson      -> JSON array of waypoints
    """

    mapReady = Signal()   # emitted once loading finishes

    def __init__(self, config: FleetConfig | None = None, parent=None):
        super().__init__(parent)

        self._image_path = ""
        self._origin_x = 0.0
        self._origin_y = 0.0
        self._resolution = 0.05
        self._px_w = 0
        self._px_h = 0
        self._waypoints = []   # list of dict
        self._lanes = []       # list of {from, to}

        maps = _maps_dir()
        self._map_yaml = maps / "map.yaml"
        self._map_png = maps / "map.png"

        # The adapter's graph wins; ours is the standalone fallback.
        adapter_graph = config.nav_graph if config else None
        self._nav_graph = adapter_graph or (maps / "nav_graph.yaml")

        self._load()

    # ── Load ─────────────────────────────────────────────────────────────────

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
        origin = data["origin"]          # [x, y, yaw]
        self._origin_x = float(origin[0])
        self._origin_y = float(origin[1])
        self._resolution = float(data["resolution"])

    def _load_png(self):
        """Read the map image dimensions; QML loads the PNG itself.

        The occupancy grid is semantic data, so it is handed to QML untouched —
        no re-encoding, and no shared temp file whose fixed name collides
        between users on the same machine.
        """
        img = QImage(str(self._map_png))
        self._px_w = img.width()
        self._px_h = img.height()
        self._image_path = str(self._map_png)

    def _read_nav_graph(self):
        """Read waypoints and lanes from the nav graph."""
        with open(self._nav_graph) as f:
            data = yaml.safe_load(f)
        levels = data["levels"]
        level = next(iter(levels.values()))   # use the first level
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

        lanes_raw = level.get("lanes", [])
        all_pairs = {(int(ln[0]), int(ln[1])) for ln in lanes_raw}
        seen, lanes = set(), []
        for ln in lanes_raw:
            a, b = int(ln[0]), int(ln[1])
            key = (min(a, b), max(a, b))
            if key not in seen:
                seen.add(key)
                lanes.append({"from": a, "to": b,
                              "bidir": (b, a) in all_pairs})
        self._lanes = lanes

    def waypoints(self) -> list:
        """Waypoints read from the nav graph, as {name, x, y, charger, parking}."""
        return self._waypoints

    # ── QML Properties ────────────────────────────────────────────────────────

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
