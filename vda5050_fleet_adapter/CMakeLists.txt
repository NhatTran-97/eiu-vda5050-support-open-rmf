cmake_minimum_required(VERSION 3.16)
project(vda5050_fleet_adapter)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmf_fleet_adapter REQUIRED)
find_package(rmf_traffic REQUIRED)
find_package(rmf_battery REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

# ─── Paho MQTT C++ (apt: libpaho-mqttpp-dev libpaho-mqtt-dev) ──────────────────
find_library(PAHO_MQTTPP_LIB NAMES paho-mqttpp3)
find_library(PAHO_MQTT_C_LIB NAMES paho-mqtt3as paho-mqtt3a)
find_path(PAHO_MQTTPP_INC NAMES mqtt/async_client.h)
if(NOT PAHO_MQTTPP_LIB OR NOT PAHO_MQTT_C_LIB OR NOT PAHO_MQTTPP_INC)
  message(FATAL_ERROR
    "Paho MQTT C++ not found. Install: sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev")
endif()

# ─── Core library (everything except the ROS entry point) ─────────────────────
add_library(${PROJECT_NAME}_core SHARED
  src/vda5050_protocol.cpp
  src/vda5050_connector.cpp
  src/robot_state_machine.cpp
  src/robot_adapter.cpp
)
target_include_directories(${PROJECT_NAME}_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  ${PAHO_MQTTPP_INC}
)
target_link_libraries(${PROJECT_NAME}_core PUBLIC
  rmf_fleet_adapter::rmf_fleet_adapter
  nlohmann_json::nlohmann_json
  yaml-cpp
  Eigen3::Eigen
  ${PAHO_MQTTPP_LIB}
  ${PAHO_MQTT_C_LIB}
)
ament_target_dependencies(${PROJECT_NAME}_core PUBLIC
  rclcpp rmf_traffic rmf_battery)

# ─── Node executable ──────────────────────────────────────────────────────────
add_executable(fleet_adapter src/main.cpp)
target_link_libraries(fleet_adapter ${PROJECT_NAME}_core)

install(TARGETS ${PROJECT_NAME}_core
  EXPORT ${PROJECT_NAME}_core
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)
install(TARGETS fleet_adapter DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config maps launch DESTINATION share/${PROJECT_NAME})
install(PROGRAMS
        scripts/dispatch_patrol.py
        scripts/cancel_task.py
        scripts/mock_mqtt_robot.py
        scripts/test_dispatch_e2e.py
        DESTINATION lib/${PROJECT_NAME})

# ─── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_protocol test/test_protocol.cpp)
  target_link_libraries(test_protocol ${PROJECT_NAME}_core)

  ament_add_gtest(test_transform test/test_transform.cpp)
  target_link_libraries(test_transform ${PROJECT_NAME}_core)

  ament_add_gtest(test_state_machine test/test_state_machine.cpp)
  target_link_libraries(test_state_machine ${PROJECT_NAME}_core)
endif()

ament_package()
