FROM osrf/ros:jazzy-desktop

LABEL maintainer="EIU - School of Engineering" \
      description="TurtleBot3 Gazebo Harmonic + Nav2 + VDA5050 bridge (standalone)" \
      version="1.1" \
      license="MIT"

ENV DEBIAN_FRONTEND=noninteractive

# ----------------------------------------------------------
# Base tools
# ----------------------------------------------------------
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    wget \
    gnupg \
    lsb-release \
    nano \
    net-tools \
    iputils-ping \
    python3 \
    python3-pip \
    python3-colcon-common-extensions \
    python3-argcomplete \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

# ----------------------------------------------------------
# Gazebo Harmonic apt repository
# ----------------------------------------------------------
RUN curl -sSL https://packages.osrfoundation.org/gazebo.gpg \
        -o /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg && \
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] \
        http://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/gazebo-stable.list

# ----------------------------------------------------------
# Simulation + Navigation + VDA5050 dependencies
# ----------------------------------------------------------
RUN apt-get update && apt-get install -y \
    gz-harmonic \
    ros-jazzy-ros-gz-sim \
    ros-jazzy-ros-gz-bridge \
    ros-jazzy-xacro \
    ros-jazzy-urdf \
    ros-jazzy-robot-state-publisher \
    ros-jazzy-joint-state-publisher-gui \
    ros-jazzy-turtlebot3-msgs \
    ros-jazzy-turtlebot3-gazebo \
    ros-jazzy-turtlebot3-teleop \
    ros-jazzy-navigation2 \
    ros-jazzy-nav2-bringup \
    ros-jazzy-nav2-route \
    ros-jazzy-tf-transformations \
    ros-jazzy-rmf-building-map-tools \
    ros-jazzy-rmf-traffic-editor \
    libpaho-mqtt-dev \
    libpaho-mqttpp-dev \
    nlohmann-json3-dev \
    mosquitto-clients \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh


RUN userdel -r ubuntu 2>/dev/null || true && \
    useradd -m -u 1000 eiu
WORKDIR /home/eiu
USER eiu

ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
ENV TURTLEBOT3_MODEL=burger

RUN echo "source /opt/ros/jazzy/setup.bash" >> /home/eiu/.bashrc && \
    echo "[ -f /home/eiu/sim_ws/install/setup.bash ] && source /home/eiu/sim_ws/install/setup.bash" >> /home/eiu/.bashrc && \
    echo "export TURTLEBOT3_MODEL=burger" >> /home/eiu/.bashrc && \
    echo "export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp" >> /home/eiu/.bashrc

ENTRYPOINT ["/entrypoint.sh"]
CMD ["/bin/bash"]
