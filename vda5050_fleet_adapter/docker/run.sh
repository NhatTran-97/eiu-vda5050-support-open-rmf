#!/usr/bin/env bash

set -euo pipefail

IMAGE="rmf_jazzy_vda"
CONTAINER="rmf_jazzy_vda_dev"
WS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   
DOMAIN=7  

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">> building image $IMAGE ..."
  docker build -t "$IMAGE" "$(dirname "${BASH_SOURCE[0]}")"
fi

echo ">> container=$CONTAINER  ws=$WS  domain=$DOMAIN  (network=host)"
# Remove any stopped container with the same name before starting a fresh one.
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
exec docker run --rm -it \
  --name "$CONTAINER" \
  --network host \
  -e ROS_DOMAIN_ID="$DOMAIN" \
  -v "$WS":/ros2_ws \
  -w /ros2_ws \
  "$IMAGE" bash
