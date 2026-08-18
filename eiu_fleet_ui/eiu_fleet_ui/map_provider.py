import json
from pathlib import Path
from PySide6.QtCore import QObject, Signal, Property
from PySide6.QtGui import QImage


_src_maps = Path(__file__).parent.parent / "maps"
if _src_maps.exists():
    _MAPS = _src_maps
else:
    from ament_index_python.packages import get_package_share_directory
    _MAPS = Path(get_package_share_directory("eiu_fleet_ui")) / "maps"

MAP_YAML  = _MAPS / "map.yaml"
MAP_PNG   = _MAPS / "map.png"
NAV_GRAPH = _MAPS / "nav_graph.yaml"


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

    def __init__(self, parent=None):
        super().__init__(parent)

        self._image_path = ""
        self._origin_x   = 0.0
        self._origin_y   = -23.6
        self._resolution = 0.05
        self._px_w       = 0
        self._px_h       = 0
        self._waypoints  = []   # list of dict
        self._lanes      = []   # list of {from, to}

        self._load()

    # ── Load ─────────────────────────────────────────────────────────────────

    def _load(self):
        """Read map.yaml, convert the map image, then read the nav graph."""

        self._read_map_yaml()
        self._load_png()
        self._read_nav_graph()
        self.mapReady.emit()
  

    def _read_map_yaml(self):
        import yaml
        with open(MAP_YAML) as f:
            data = yaml.safe_load(f)
        origin = data["origin"]          # [0.0, -23.6, 0.0]
        self._origin_x   = float(origin[0])
        self._origin_y   = float(origin[1])
        self._resolution = float(data["resolution"])


    def _load_png(self):
        """
        Load the map PNG and convert it to ARGB32 so QML can render it.
        The source map.png is Grayscale8, a format Qt Quick cannot display directly.
        """
        import tempfile, os
        img = QImage(str(MAP_PNG))
        img = img.convertToFormat(QImage.Format_ARGB32)
        self._px_w = img.width()
        self._px_h = img.height()
        # Cache the converted ARGB32 image to a temp file for QML to load
        cache = os.path.join(tempfile.gettempdir(), "eiu_fleet_map.png")
        img.save(cache)
        self._image_path = cache

    def _read_nav_graph(self):
        """Read waypoints and lanes from nav_graph.yaml."""

        import yaml
        with open(NAV_GRAPH) as f:
            data = yaml.safe_load(f)
        levels   = data["levels"]
        level    = next(iter(levels.values()))   # use the first level
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

        lanes_raw  = level.get("lanes", [])
        all_pairs  = {(int(ln[0]), int(ln[1])) for ln in lanes_raw}
        seen, lanes = set(), []
        for ln in lanes_raw:
            a, b = int(ln[0]), int(ln[1])
            key  = (min(a, b), max(a, b))
            if key not in seen:
                seen.add(key)
                lanes.append({"from": a, "to": b,
                               "bidir": (b, a) in all_pairs})
        self._lanes = lanes
      

    # ── QML Properties ────────────────────────────────────────────────────────

    
    @Property(str, notify=mapReady)
    def imagePath(self):
        return f"file://{self._image_path}" if self._image_path else ""

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
