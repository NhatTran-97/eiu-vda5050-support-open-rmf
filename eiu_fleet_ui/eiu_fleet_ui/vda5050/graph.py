"""Distance of a robot pose from the navigation graph, and sustained-deviation tracking."""

from math import hypot


class NavGraph:
    """Lane segments of the navigation graph, in the same frame as the robot pose."""

    def __init__(self, waypoints: list, lanes: list):
        points = [(float(w["x"]), float(w["y"])) for w in waypoints]
        self._segments = [(points[ln["from"]], points[ln["to"]]) for ln in lanes
                          if ln["from"] < len(points) and ln["to"] < len(points)]

    def distance(self, x: float, y: float) -> float | None:
        """Distance in metres to the nearest lane; None when the graph has no lanes."""
        best = None
        for (ax, ay), (bx, by) in self._segments:
            dx, dy = bx - ax, by - ay
            length_sq = dx * dx + dy * dy
            t = 0.0 if length_sq == 0 else max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / length_sq))
            d = hypot(x - (ax + t * dx), y - (ay + t * dy))
            best = d if best is None or d < best else best
        return best


class OffGraphTracker:
    """Flags robots that stay farther than `limit` metres from the graph for `hold` seconds."""

    def __init__(self, graph: NavGraph, limit: float = 1.2, hold: float = 5.0):
        self._graph = graph
        self._limit = limit
        self._hold = hold
        self._since: dict[str, float] = {}

    def update(self, name: str, x, y, now: float) -> tuple[float | None, bool]:
        """Return (distance to the graph, whether the deviation is sustained)."""
        if x is None or y is None:
            self._since.pop(name, None)
            return None, False
        d = self._graph.distance(x, y)
        if d is None or d <= self._limit:
            self._since.pop(name, None)
            return d, False
        start = self._since.setdefault(name, now)
        return d, now - start >= self._hold
