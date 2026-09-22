#!/bin/bash
set -e
source /opt/ros/jazzy/setup.bash
[ -f /home/eiu/install/setup.bash ] && source /home/eiu/install/setup.bash
exec "$@"
