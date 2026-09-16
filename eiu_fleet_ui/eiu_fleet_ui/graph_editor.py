"""Interactive nav_graph.yaml editor: vertices + lanes, load/new/save.

Mirrors Open-RMF's own nav_graph.yaml schema (the same format
traffic_editor produces) so a graph edited here loads straight into RMF:

    building_name: <str>
    doors: {}
    levels:
      <level_name>:
        vertices: [[x, y, {name: ..., is_charger: true, ...}], ...]
        lanes:    [[from_index, to_index, {...params}], ...]
    lifts: {}

A lane is one direction only -- a bidirectional connection is two entries
(A->B and B->A), same convention as the files this project already ships.
"""

import json
from pathlib import Path

import yaml
from PySide6.QtCore import QObject, Signal, Slot, Property


class GraphEditor(QObject):
    """Holds one editable working copy of a nav graph, exposed to QML."""

    graphChanged = Signal()
    saveResult = Signal(bool, str)   # ok, message
    loadResult = Signal(bool, str)   # ok, message

    def __init__(self, parent=None):
        super().__init__(parent)
        self._active = False
        self._vertices = []   # [{x, y, name, is_charger, is_holding_point}]
        self._lanes = []      # [{from, to, params}] -- one direction per entry
        self._building_name = "map"
        self._level_name = "level1"
        self._source_path = ""

    # ── Mode ──────────────────────────────────────────────────────────────

    @Slot(str, str)
    def loadFromFile(self, path: str, level_hint: str = ""):
        """Load an existing nav_graph.yaml into the editable working copy."""
        try:
            data = yaml.safe_load(Path(path).read_text()) or {}
        except Exception as e:
            self.loadResult.emit(False, f"Could not read {path}: {e}")
            return

        levels = data.get("levels") or {}
        if not levels:
            self.loadResult.emit(False, f"No levels found in {path}")
            return
        level_name = level_hint if level_hint in levels else next(iter(levels))
        level = levels[level_name] or {}

        vertices = []
        for v in level.get("vertices") or []:
            if len(v) < 2:
                continue
            x, y = float(v[0]), float(v[1])
            params = v[2] if len(v) > 2 and isinstance(v[2], dict) else {}
            vertices.append({
                "x": x, "y": y,
                "name": str(params.get("name", "")),
                "is_charger": bool(params.get("is_charger", False)),
                "is_holding_point": bool(params.get("is_holding_point", False)),
            })

        lanes = []
        for l in level.get("lanes") or []:
            if len(l) < 2:
                continue
            params = l[2] if len(l) > 2 and isinstance(l[2], dict) else {}
            lanes.append({"from": int(l[0]), "to": int(l[1]), "params": dict(params)})

        self._building_name = str(data.get("building_name") or self._building_name)
        self._level_name = level_name
        self._vertices = vertices
        self._lanes = lanes
        self._source_path = path
        self._active = True
        self.graphChanged.emit()
        self.loadResult.emit(True, f"Loaded {len(vertices)} waypoints, "
                             f"{len(lanes)} lane entries from {path}")

    @Slot()
    @Slot(str)
    def newGraph(self, building_name: str = ""):
        """Start a blank graph (map background stays; only the graph is cleared)."""
        self._vertices = []
        self._lanes = []
        self._source_path = ""
        if building_name:
            self._building_name = building_name
        self._active = True
        self.graphChanged.emit()

    @Slot()
    def stopEditing(self):
        self._active = False
        self.graphChanged.emit()

    # ── Vertices ──────────────────────────────────────────────────────────

    @Slot(float, float, str, bool, result=int)
    def addVertex(self, x: float, y: float, name: str, is_charger: bool) -> int:
        self._vertices.append({
            "x": x, "y": y, "name": name.strip(),
            "is_charger": is_charger, "is_holding_point": False,
        })
        self.graphChanged.emit()
        return len(self._vertices) - 1

    @Slot(int, str)
    def renameVertex(self, index: int, name: str):
        if 0 <= index < len(self._vertices):
            self._vertices[index]["name"] = name.strip()
            self.graphChanged.emit()

    @Slot(int, bool)
    def setVertexCharger(self, index: int, is_charger: bool):
        if 0 <= index < len(self._vertices):
            self._vertices[index]["is_charger"] = is_charger
            self.graphChanged.emit()

    @Slot(int, float, float)
    def moveVertex(self, index: int, x: float, y: float):
        if 0 <= index < len(self._vertices):
            self._vertices[index]["x"] = x
            self._vertices[index]["y"] = y
            self.graphChanged.emit()

    @Slot(int)
    def removeVertex(self, index: int):
        if not (0 <= index < len(self._vertices)):
            return
        del self._vertices[index]
        kept = []
        for l in self._lanes:
            if l["from"] == index or l["to"] == index:
                continue  # drop lanes touching the removed vertex
            frm = l["from"] - 1 if l["from"] > index else l["from"]
            to = l["to"] - 1 if l["to"] > index else l["to"]
            kept.append({"from": frm, "to": to, "params": l["params"]})
        self._lanes = kept
        self.graphChanged.emit()

    # ── Lanes ─────────────────────────────────────────────────────────────

    def _lane_exists(self, frm: int, to: int) -> bool:
        return any(l["from"] == frm and l["to"] == to for l in self._lanes)

    @Slot(int, int, bool)
    def addLane(self, from_idx: int, to_idx: int, bidirectional: bool):
        n = len(self._vertices)
        if from_idx == to_idx or not (0 <= from_idx < n) or not (0 <= to_idx < n):
            return
        changed = False
        if not self._lane_exists(from_idx, to_idx):
            self._lanes.append({"from": from_idx, "to": to_idx, "params": {}})
            changed = True
        if bidirectional and not self._lane_exists(to_idx, from_idx):
            self._lanes.append({"from": to_idx, "to": from_idx, "params": {}})
            changed = True
        if changed:
            self.graphChanged.emit()

    @Slot(int, int)
    def removeLane(self, a: int, b: int):
        """Remove both directions between a and b (a single visual lane)."""
        before = len(self._lanes)
        self._lanes = [l for l in self._lanes
                       if not ((l["from"] == a and l["to"] == b)
                               or (l["from"] == b and l["to"] == a))]
        if len(self._lanes) != before:
            self.graphChanged.emit()

    # ── Save ──────────────────────────────────────────────────────────────

    @Slot(str, result=bool)
    def saveAs(self, path: str) -> bool:
        try:
            named = [v["name"] for v in self._vertices if v["name"]]
            if len(named) != len(set(named)):
                self.saveResult.emit(False, "Duplicate waypoint names — fix before saving")
                return False
            if not path.strip():
                self.saveResult.emit(False, "No filename given")
                return False

            vertices_yaml = []
            for v in self._vertices:
                params = {}
                if v["name"]:
                    params["name"] = v["name"]
                if v.get("is_charger"):
                    params["is_charger"] = True
                if v.get("is_holding_point"):
                    params["is_holding_point"] = True
                vertices_yaml.append([v["x"], v["y"], params])

            lanes_yaml = [[l["from"], l["to"], dict(l["params"] or {})] for l in self._lanes]

            data = {
                "building_name": self._building_name,
                "doors": {},
                "levels": {
                    self._level_name: {
                        "vertices": vertices_yaml,
                        "lanes": lanes_yaml,
                    },
                },
                "lifts": {},
            }

            out = Path(path)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(yaml.safe_dump(data, sort_keys=False, default_flow_style=None))
            self._source_path = path
            self.saveResult.emit(True, f"Saved {len(vertices_yaml)} waypoints, "
                                 f"{len(lanes_yaml)} lane entries to {path}")
            return True
        except Exception as e:
            self.saveResult.emit(False, f"Save failed: {e}")
            return False

    # ── QML-facing state ─────────────────────────────────────────────────

    @Property(bool, notify=graphChanged)
    def active(self):
        return self._active

    @Property(str, notify=graphChanged)
    def sourcePath(self):
        return self._source_path

    @Property(str, notify=graphChanged)
    def verticesJson(self):
        return json.dumps([
            {"index": i, "x": v["x"], "y": v["y"], "name": v["name"],
             "is_charger": v["is_charger"]}
            for i, v in enumerate(self._vertices)
        ])

    @Property(str, notify=graphChanged)
    def lanesJson(self):
        # One entry per unordered pair, with a bidir flag -- matches how
        # MapPage already draws the live RMF graph (see root.edges).
        seen = {}
        order = []
        for l in self._lanes:
            key = tuple(sorted((l["from"], l["to"])))
            if key not in seen:
                seen[key] = {"from": l["from"], "to": l["to"], "bidir": False}
                order.append(key)
            if self._lane_exists(l["to"], l["from"]):
                seen[key]["bidir"] = True
        return json.dumps([seen[k] for k in order])
