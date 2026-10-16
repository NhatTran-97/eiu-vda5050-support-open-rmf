# Build environment for the Jazzy VDA5050 fleet adapter.
# Build with ./build.sh (it adds the web backend folder, or an empty folder, as the build context "backend").

FROM ros:jazzy-ros-base

ENV DEBIAN_FRONTEND=noninteractive

# Install Open-RMF and build dependencies.

RUN apt-get update && apt-get install -y --no-install-recommends \
      ros-jazzy-rmf-fleet-adapter \
      ros-jazzy-rmf-traffic-ros2 \
      ros-jazzy-rmf-task-ros2 \
      ros-jazzy-rmf-task-msgs \
      ros-jazzy-rmf-building-map-tools \
      ros-jazzy-nav2-msgs \
      libpaho-mqtt-dev \
      libpaho-mqttpp-dev \
      nlohmann-json3-dev \
      libyaml-cpp-dev \
      libeigen3-dev \
      python3-colcon-common-extensions \
      python3-paho-mqtt \
      python3-yaml \
      python3-jsonschema \
      mosquitto-clients \
    && rm -rf /var/lib/apt/lists/*

# Install the EIU fleet UI dependencies.
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

# Install Redis and its Python client for the optional web dashboard.
RUN apt-get update && apt-get install -y --no-install-recommends \
      redis-server \
      python3-redis \
    && rm -rf /var/lib/apt/lists/*

# Install the WebSocket library for the optional web dashboard.
RUN pip install --no-cache-dir --break-system-packages "websockets>=12"

# Install the web dashboard backend dependencies when the build context "backend" has a requirements.txt.
# The context is mounted for this step only, so no backend file is kept in the image.
RUN --mount=type=bind,from=backend,target=/tmp/eiu_web_backend \
    if [ -f /tmp/eiu_web_backend/requirements.txt ]; then \
      pip install --no-cache-dir --break-system-packages -r /tmp/eiu_web_backend/requirements.txt; \
    fi

# Source ROS and the mounted workspace overlay in each shell.
RUN echo 'source /opt/ros/jazzy/setup.bash' >> /root/.bashrc \
 && echo '[ -f /ros2_ws/install/setup.bash ] && source /ros2_ws/install/setup.bash' \
      >> /root/.bashrc

WORKDIR /ros2_ws
CMD ["bash"]
