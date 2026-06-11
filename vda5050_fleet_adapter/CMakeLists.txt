cmake_minimum_required(VERSION 3.8)
project(vda5050_fleet_adapter)

set(CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmf_fleet_adapter REQUIRED)
find_package(rmf_traffic REQUIRED)
find_package(rmf_traffic_ros2 REQUIRED)
find_package(rmf_battery REQUIRED)
find_package(PahoMqttCpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)

add_executable(vda5050_fleet_adapter
  src/main.cpp
  src/fleet_adapter.cpp
  src/robot_command_handle.cpp
)

target_include_directories(vda5050_fleet_adapter
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

ament_target_dependencies(vda5050_fleet_adapter
  rclcpp
  rmf_traffic
)

target_link_libraries(vda5050_fleet_adapter
  rmf_fleet_adapter::rmf_fleet_adapter
  rmf_traffic_ros2::rmf_traffic_ros2
  rmf_battery::rmf_battery
  PahoMqttCpp::paho-mqttpp3
  nlohmann_json::nlohmann_json
  yaml-cpp
)

install(TARGETS vda5050_fleet_adapter
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY maps/
  DESTINATION share/${PROJECT_NAME}/maps
)

install(DIRECTORY config/
  DESTINATION share/${PROJECT_NAME}/config
)

install(DIRECTORY launch/
  DESTINATION share/${PROJECT_NAME}/launch
)

install(PROGRAMS scripts/visualize_nav_graph.py
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
