#!/bin/bash
set -e
source /opt/ros/jazzy/setup.bash
[ -f /home/eiu/sim_ws/install/setup.bash ] && source /home/eiu/sim_ws/install/setup.bash
exec "$@"
