#!/usr/bin/env bash
# Build the image rmf_jazzy_vda. The web backend folder is a second build context, so the image installs the
# backend's own requirements.txt (the same file a local virtual environment uses).
#   ./build.sh               build, reusing cached layers
#   ./build.sh --no-cache    rebuild every layer

set -euo pipefail

IMAGE="rmf_jazzy_vda"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND="$HERE/../../eiu_fleet_ui/web_dashboard/backend"

docker build "$@" --build-context backend="$BACKEND" -t "$IMAGE" "$HERE"
