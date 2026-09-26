#!/usr/bin/env bash
# Start one virtual AGV (virtual_agv.js) per named waypoint, placed at that waypoint of the nav graph.
# Usage: run_virtual_fleet.sh [waypoint ...]   (default: charger_1 charger_2 charger_3)
# The Nth waypoint gets serial 000N, matching virtual_N in config_virtual_agv.yaml. Ctrl+C stops them all.
# Environment: AGV_BROKER (mqtt://127.0.0.1:18830), AGV_SPEED (0.3, below rmf_fleet.limits.linear so the AGV is never
#              ahead of RMF's schedule: it turns and accelerates instantly), AGV_MANUFACTURER (THIRDPARTY),
#              NAV_GRAPH (../../maps/nav_graph.yaml), NODE_IMAGE (node:20-alpine).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BROKER="${AGV_BROKER:-mqtt://127.0.0.1:18830}"
SPEED="${AGV_SPEED:-0.3}"
MANUFACTURER="${AGV_MANUFACTURER:-THIRDPARTY}"
GRAPH="${NAV_GRAPH:-$HERE/../../maps/nav_graph.yaml}"
IMAGE="${NODE_IMAGE:-node:20-alpine}"
if [ "$#" -gt 0 ]; then WAYPOINTS=("$@"); else WAYPOINTS=(charger_1 charger_2 charger_3); fi

run_node() {
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$HERE:/w" -w /w "$@"
}

# Print "x y level" of a named waypoint of the nav graph.
waypoint_pose() {
    python3 - "$GRAPH" "$1" <<'PY'
import sys, yaml
graph, name = yaml.safe_load(open(sys.argv[1])), sys.argv[2]
for level, content in (graph.get("levels") or {}).items():
    for vertex in content.get("vertices") or []:
        if len(vertex) > 2 and (vertex[2] or {}).get("name") == name:
            print(vertex[0], vertex[1], level)
            sys.exit(0)
sys.exit(f"waypoint '{name}' is not in {sys.argv[1]}")
PY
}

[ -d "$HERE/node_modules/vda-5050-lib" ] || run_node "$IMAGE" npm install --no-audit --no-fund

NAMES=()
stop_all() {
    if [ "${#NAMES[@]}" -gt 0 ]; then
        docker rm -f "${NAMES[@]}" >/dev/null 2>&1 || true
    fi
    echo "virtual AGVs stopped"
}
trap stop_all EXIT
trap 'exit 0' INT TERM

index=0
for waypoint in "${WAYPOINTS[@]}"; do
    index=$((index + 1))
    read -r x y level <<<"$(waypoint_pose "$waypoint")"
    serial="$(printf '%04d' "$index")"
    name="virtual_agv_$serial"
    docker rm -f "$name" >/dev/null 2>&1 || true
    docker run -d --name "$name" --network host -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$HERE:/w" -w /w \
        -e AGV_BROKER="$BROKER" -e AGV_MANUFACTURER="$MANUFACTURER" -e AGV_SERIAL="$serial" \
        -e AGV_X="$x" -e AGV_Y="$y" -e AGV_NODE="$waypoint" -e AGV_MAP="$level" -e AGV_SPEED="$SPEED" \
        "$IMAGE" node virtual_agv.js >/dev/null
    NAMES+=("$name")
    echo "$name: $MANUFACTURER/$serial at $waypoint ($x, $y) on $level"
done

for name in "${NAMES[@]}"; do
    docker logs -f "$name" 2>&1 | sed "s/^/[$name] /" &
done
wait
