FROM ros:jazzy-ros-base

# Install system dependencies
RUN apt-get update && apt-get install -y \
    libpaho-mqtt-dev \
    libpaho-mqttpp-dev \
    python3-colcon-common-extensions \
    mosquitto-clients \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

# Create workspace
WORKDIR /ros2_ws

# Copy packages
# Build context must be the src/ directory:
#   docker build -f vda5050_client_adapter/Dockerfile -t vda5050-client-adapter .
COPY vda5050_msgs/           src/vda5050_msgs/
COPY vda5050_client_adapter/ src/vda5050_client_adapter/

# Build vda5050_msgs first, then the adapter
RUN . /opt/ros/jazzy/setup.sh && \
    colcon build \
      --packages-select vda5050_msgs vda5050_client_adapter \
      --cmake-args -DCMAKE_BUILD_TYPE=Release

# Source workspace on container start
RUN echo "source /opt/ros/jazzy/setup.bash" >> /root/.bashrc && \
    echo "source /ros2_ws/install/setup.bash" >> /root/.bashrc

COPY vda5050_client_adapter/docker-entrypoint.sh /docker-entrypoint.sh
RUN chmod +x /docker-entrypoint.sh

ENV PARAMS_FILE=/ros2_ws/install/vda5050_client_adapter/share/vda5050_client_adapter/config/vda5050_params.yaml

ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["ros2", "run", "vda5050_client_adapter", "vda5050_client_adapter_node", "--ros-args", "--params-file", "/ros2_ws/install/vda5050_client_adapter/share/vda5050_client_adapter/config/vda5050_params.yaml"]
