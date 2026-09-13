# Jazzy + Open-RMF + Paho MQTT C++ environment for the VDA5050 fleet adapter.

FROM ros:jazzy-ros-base

ENV DEBIAN_FRONTEND=noninteractive

# Open-RMF (fleet adapter + core nodes) and build/runtime deps. 
# nav2-msgs isn't pulled in by the RMF packages above but is needed to build tb3_vda5050_bridge (also compiled/tested here) via nav2_msgs::SpeedLimit.

RUN apt-get update && apt-get install -y --no-install-recommends \
      ros-jazzy-rmf-fleet-adapter \
      ros-jazzy-rmf-traffic-ros2 \
      ros-jazzy-rmf-task-ros2 \
      ros-jazzy-rmf-task-msgs \
      ros-jazzy-nav2-msgs \
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
