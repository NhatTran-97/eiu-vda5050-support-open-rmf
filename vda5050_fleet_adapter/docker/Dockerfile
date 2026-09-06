# Jazzy + Open-RMF + Paho MQTT C++ environment for the VDA5050 fleet adapter.
# The workspace is MOUNTED at run time (see run.sh) so you can rebuild without
# rebuilding the image.
FROM ros:jazzy-ros-base

ENV DEBIAN_FRONTEND=noninteractive

# Open-RMF (fleet adapter + core nodes) and build/runtime deps.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ros-jazzy-rmf-fleet-adapter \
      ros-jazzy-rmf-traffic-ros2 \
      ros-jazzy-rmf-task-ros2 \
      ros-jazzy-rmf-task-msgs \
      libpaho-mqtt-dev \
      libpaho-mqttpp-dev \
      nlohmann-json3-dev \
      libyaml-cpp-dev \
      libeigen3-dev \
      python3-colcon-common-extensions \
      python3-paho-mqtt \
      python3-yaml \
      mosquitto-clients \
    && rm -rf /var/lib/apt/lists/*

# ── eiu_fleet_ui (PySide6/QML GUI) ──────────────────────────────────────────
# Runs inside this Jazzy container instead of the host so it shares one ROS2
# distro with the fleet adapter/RMF core — mixing Jazzy and Humble rclpy on
# the same DDS domain corrupts message deserialization (CDR layout differs
# between distros), which is what broke /fleet_states, /task_api_responses
# and /dispatch_states on the host-Humble UI.
#
# PySide6 comes from pip (not apt) so every Qt6/QML module the UI needs
# (QtQuick.Layouts, QtQuick.Controls, ...) is guaranteed present as one
# matched set, instead of chasing individual qml6-module-* apt packages.
# The xcb/GL libraries below are what Qt's "xcb" platform plugin needs to
# actually open a window against an X11 DISPLAY forwarded from the host
# (see docker/run.sh).
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3-pip \
      libgl1 \
      libegl1 \
      libopengl0 \
      libxkbcommon-x11-0 \
      libxcb-cursor0 \
      libxcb-icccm4 \
      libxcb-image0 \
      libxcb-keysyms1 \
      libxcb-randr0 \
      libxcb-render-util0 \
      libxcb-shape0 \
      libxcb-xinerama0 \
      libxcb-xkb1 \
      libx11-xcb1 \
      libdbus-1-3 \
      libfontconfig1 \
      libfreetype6 \
      libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir --break-system-packages PySide6

# Source ROS + the (mounted) workspace overlay for every shell.
RUN echo 'source /opt/ros/jazzy/setup.bash' >> /root/.bashrc \
 && echo '[ -f /ros2_ws/install/setup.bash ] && source /ros2_ws/install/setup.bash' \
      >> /root/.bashrc

WORKDIR /ros2_ws
CMD ["bash"]
