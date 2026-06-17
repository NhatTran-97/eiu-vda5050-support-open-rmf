#!/bin/bash
set -e

# Verify that an X display is available before starting the container.
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

# Allow local processes (the container) to connect to the X server.
echo ">>> Granting X11 access to local connections"
xhost +local:

cleanup() {
  echo ""
  echo ">>> Stopping Docker Compose..."
  docker compose down --remove-orphans
  echo ">>> Revoking X11 access"
  xhost -local:
}
trap cleanup INT TERM

echo ">>> Starting tb3-simulation container..."
docker compose up
cleanup
