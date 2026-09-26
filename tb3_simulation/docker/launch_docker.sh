#!/bin/bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="${SCRIPT_DIR}/docker-compose.yaml"
CONTAINER_NAME="tb3-simulation"
CLEANED_UP=0

# Require an active X display.
if [ -z "$DISPLAY" ]; then
  echo "ERROR: DISPLAY is not set. Run this script inside an X session."
  exit 1
fi

if [ ! -S "/tmp/.X11-unix/X${DISPLAY#:}" ]; then
  echo "ERROR: X11 socket not found: /tmp/.X11-unix/X${DISPLAY#:}"
  exit 1
fi

echo ">>> DISPLAY=$DISPLAY — X11 socket OK"

export DISPLAY
export XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}"
touch "$XAUTHORITY"
mkdir -p "$HOME/.config/open-robotics" "$HOME/.cache/open-robotics"

# Allow the container to connect to the local X server.
echo ">>> Granting X11 access to local connections"
xhost +local:

cleanup() {
  if [ "$CLEANED_UP" -eq 1 ]; then
    return
  fi
  CLEANED_UP=1

  echo ""
  echo ">>> Stopping and removing ${CONTAINER_NAME}..."
  docker compose -f "$COMPOSE_FILE" down --remove-orphans || true
  echo ">>> Revoking X11 access"
  xhost -local: || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo ">>> Removing any old ${CONTAINER_NAME} container..."
docker compose -f "$COMPOSE_FILE" down --remove-orphans

echo ">>> Starting a fresh ${CONTAINER_NAME} container..."
docker compose -f "$COMPOSE_FILE" up -d --force-recreate

echo ">>> Entering ${CONTAINER_NAME}. Type 'exit' or press Ctrl+D to stop it."
docker exec -it "$CONTAINER_NAME" bash
