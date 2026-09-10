cmake_minimum_required(VERSION 3.8)
project(vda5050_fleet_adapter_full_control)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
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
  message(FATAL_ERROR "Paho MQTT C++ not found. Install: sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev")
endif()


# ─── Core library  ─────────────────────
add_library(${PROJECT_NAME}_core SHARED
  src/mqtt/mqtt_client.cpp
  src/vda5050/message_builder.cpp
  src/vda5050/state_handler.cpp
  src/vda5050/order_handler.cpp
  src/vda5050/instant_action_handler.cpp
  src/rmf/connector.cpp
  src/rmf/robot_activity_state_machine.cpp
  src/core/robot.cpp
  src/core/config.cpp
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
add_executable(fleet_adapter src/main.cpp src/core/fleet_adapter.cpp)
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


if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  set(ament_cmake_copyright_FOUND TRUE)

  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
