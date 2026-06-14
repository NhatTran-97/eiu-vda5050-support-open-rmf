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
      mosquitto-clients \
    && rm -rf /var/lib/apt/lists/*

# Source ROS + the (mounted) workspace overlay for every shell.
RUN echo 'source /opt/ros/jazzy/setup.bash' >> /root/.bashrc \
 && echo '[ -f /ros2_ws/install/setup.bash ] && source /ros2_ws/install/setup.bash' \
      >> /root/.bashrc

WORKDIR /ros2_ws
CMD ["bash"]
