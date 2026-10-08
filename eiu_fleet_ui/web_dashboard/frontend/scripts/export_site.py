#!/usr/bin/env python3
"""Export the occupancy map and nav graph of eiu_fleet_ui into the demo data of the web frontend.

Writes src/mocks/data/site.json (levels, map metadata, graph vertices and lanes) and
public/demo/maps/<level>.png (the map image re-encoded as RGBA PNG).
"""

import argparse
import json
from pathlib import Path

import yaml
from PIL import Image

FRONTEND = Path(__file__).resolve().parent.parent
DEFAULT_MAPS = FRONTEND.parent.parent / "maps"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map-yaml", type=Path, default=DEFAULT_MAPS / "map.yaml")
    parser.add_argument("--nav-graph", type=Path, default=DEFAULT_MAPS / "nav_graph.yaml")
    parser.add_argument("--level", help="nav graph level the map image belongs to (default: the only level)")
    return parser.parse_args()


def read_map(map_yaml: Path):
    meta = yaml.safe_load(map_yaml.read_text())
    image = Image.open(map_yaml.parent / meta["image"]).convert("RGBA")
    origin = meta["origin"]
    return image, [float(origin[0]), float(origin[1])], float(meta["resolution"])


def read_graph(nav_graph: Path) -> dict:
    levels = yaml.safe_load(nav_graph.read_text())["levels"]
    graphs = {}
    for level_id, level in levels.items():
        vertices = []
        for x, y, attrs in level.get("vertices", []):
            attrs = attrs or {}
            vertices.append({
                "name": attrs.get("name", ""),
                "x": round(float(x), 4),
                "y": round(float(y), 4),
                "charger": bool(attrs.get("is_charger", False)),
            })
        lanes = [[int(a), int(b)] for a, b, *_ in level.get("lanes", [])]
        graphs[level_id] = {"vertices": vertices, "lanes": lanes}
    return graphs


def main():
    args = parse_args()
    graphs = read_graph(args.nav_graph)
    level_id = args.level or next(iter(graphs))
    if level_id not in graphs:
        raise SystemExit(f"level {level_id!r} not in {args.nav_graph}")
    if args.level is None and len(graphs) > 1:
        raise SystemExit(f"{args.nav_graph} has several levels, pass --level")

    image, origin, resolution = read_map(args.map_yaml)
    image_rel = f"demo/maps/{level_id}.png"
    image_out = FRONTEND / "public" / image_rel
    image_out.parent.mkdir(parents=True, exist_ok=True)
    image.save(image_out, optimize=True)

    site = {
        "source": {"mapYaml": args.map_yaml.name, "navGraph": args.nav_graph.name},
        "levels": [{
            "id": level_id,
            "image": image_rel,
            "origin": origin,
            "resolution": resolution,
            "widthPx": image.width,
            "heightPx": image.height,
            "graph": graphs[level_id],
        }],
    }
    out = FRONTEND / "src" / "mocks" / "data" / "site.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(site, indent=2) + "\n")
    print(f"wrote {out.relative_to(FRONTEND)} and {image_out.relative_to(FRONTEND)} "
          f"({len(graphs[level_id]['vertices'])} vertices, {len(graphs[level_id]['lanes'])} lanes)")


if __name__ == "__main__":
    main()
