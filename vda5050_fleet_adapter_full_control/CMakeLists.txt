cmake_minimum_required(VERSION 3.8)
project(vda5050_fleet_adapter_full_control)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find ROS and third-party dependencies.
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rmf_fleet_adapter REQUIRED)
find_package(rmf_traffic REQUIRED)
find_package(rmf_traffic_ros2 REQUIRED)
find_package(rmf_battery REQUIRED)
find_package(rmf_fleet_msgs REQUIRED)
find_package(rmf_utils REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(rcl_interfaces REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

# Find Paho MQTT C++.
find_library(PAHO_MQTTPP_LIB NAMES paho-mqttpp3)
find_library(PAHO_MQTT_C_LIB NAMES paho-mqtt3as paho-mqtt3a)
find_path(PAHO_MQTTPP_INC NAMES mqtt/async_client.h)
if(NOT PAHO_MQTTPP_LIB OR NOT PAHO_MQTT_C_LIB OR NOT PAHO_MQTTPP_INC)
  message(FATAL_ERROR "Paho MQTT C++ not found. Install: sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev")
endif()


# Build the adapter library.
add_library(${PROJECT_NAME}_core SHARED
  src/mqtt/mqtt_client.cpp
  src/vda5050/message_builder.cpp
  src/vda5050/state_handler.cpp
  src/vda5050/factsheet_handler.cpp
  src/vda5050/order_handler.cpp
  src/vda5050/order_validation.cpp
  src/vda5050/json_read.cpp
  src/vda5050/route_stitch.cpp
  src/vda5050/state_sequence.cpp
  src/vda5050/cancel_tracker.cpp
  src/util/log_throttle.cpp
  src/util/loop_pacer.cpp
  src/util/metrics.cpp
  src/vda5050/instant_action_handler.cpp
  src/rmf/connector.cpp
  src/rmf/robot_command_handle.cpp
  src/rmf/position_update.cpp
  src/core/config.cpp
  src/core/metrics_report.cpp
  src/core/operator_interface.cpp
  src/core/robot_registration.cpp
  src/core/runtime_robots.cpp
  src/core/robot_manager.cpp
  src/core/registration_interface.cpp
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
  rclcpp rmf_traffic rmf_traffic_ros2 rmf_battery rmf_fleet_msgs rmf_utils geometry_msgs std_msgs std_srvs rcl_interfaces)

# Build the adapter node.
add_executable(fleet_adapter
  src/main.cpp
  src/core/fleet_adapter_full_control.cpp
)
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
        scripts/dispatch_delivery.py
        scripts/cancel_task.py
        scripts/register_robot.py
        scripts/registration_sandbox.py
        scripts/mock_mqtt_robot.py
        scripts/mock_dispenser.py
        scripts/mock_ingestor.py
        scripts/test_dispatch_e2e.py
        scripts/test_pause_resume.py
        DESTINATION lib/${PROJECT_NAME})


if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  set(ament_cmake_copyright_FOUND TRUE)

  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_order_validation test/test_order_validation.cpp)
  target_link_libraries(test_order_validation ${PROJECT_NAME}_core)
  ament_add_gtest(test_route_stitch test/test_route_stitch.cpp)
  target_link_libraries(test_route_stitch ${PROJECT_NAME}_core)
  ament_add_gtest(test_robot_registration test/test_robot_registration.cpp)
  target_link_libraries(test_robot_registration ${PROJECT_NAME}_core)
  ament_add_gtest(test_runtime_robots test/test_runtime_robots.cpp)
  target_link_libraries(test_runtime_robots ${PROJECT_NAME}_core)
  ament_add_gtest(test_connector_discovery test/test_connector_discovery.cpp)
  target_link_libraries(test_connector_discovery ${PROJECT_NAME}_core)
  ament_add_gtest(test_cancel_tracker test/test_cancel_tracker.cpp)
  target_link_libraries(test_cancel_tracker ${PROJECT_NAME}_core)
  ament_add_gtest(test_loop_pacer test/test_loop_pacer.cpp)
  target_link_libraries(test_loop_pacer ${PROJECT_NAME}_core)
  ament_add_gtest(test_metrics test/test_metrics.cpp)
  target_link_libraries(test_metrics ${PROJECT_NAME}_core)
  ament_add_gtest(test_metrics_report test/test_metrics_report.cpp)
  target_link_libraries(test_metrics_report ${PROJECT_NAME}_core)
  ament_add_gtest(test_log_throttle test/test_log_throttle.cpp)
  target_link_libraries(test_log_throttle ${PROJECT_NAME}_core)
  ament_add_gtest(test_mqtt_client test/test_mqtt_client.cpp)
  target_link_libraries(test_mqtt_client ${PROJECT_NAME}_core)
  ament_add_gtest(test_state_sequence test/test_state_sequence.cpp)
  target_link_libraries(test_state_sequence ${PROJECT_NAME}_core)
  ament_add_gtest(test_connector_routing test/test_connector_routing.cpp)
  target_link_libraries(test_connector_routing ${PROJECT_NAME}_core)
  ament_add_gtest(test_registration_config test/test_registration_config.cpp)
  target_link_libraries(test_registration_config ${PROJECT_NAME}_core)
  ament_add_gtest(test_mqtt_config test/test_mqtt_config.cpp)
  target_link_libraries(test_mqtt_config ${PROJECT_NAME}_core)
  ament_add_gtest(test_threshold_config test/test_threshold_config.cpp)
  target_link_libraries(test_threshold_config ${PROJECT_NAME}_core)

  ament_add_gtest(test_input_robustness test/test_input_robustness.cpp)
  target_link_libraries(test_input_robustness ${PROJECT_NAME}_core)
  ament_add_gtest(test_position_update test/test_position_update.cpp)
  target_link_libraries(test_position_update ${PROJECT_NAME}_core)
  ament_add_gtest(test_order_invariants test/test_order_invariants.cpp)
  target_link_libraries(test_order_invariants ${PROJECT_NAME}_core)
  # Needs VDA5050_TEST_BROKER (and VDA5050_TEST_MOSQUITTO for the reconnect test); skips without it.
  ament_add_gtest(test_command_handle_broker test/test_command_handle_broker.cpp)
  target_link_libraries(test_command_handle_broker ${PROJECT_NAME}_core)
  ament_add_gtest(test_order_recovery test/test_order_recovery.cpp)
  target_link_libraries(test_order_recovery ${PROJECT_NAME}_core)
  # Benchmarks: built with the tests, run by hand.
  ament_add_gtest_executable(test_performance test/test_performance.cpp)
  target_link_libraries(test_performance ${PROJECT_NAME}_core)

  # Load-test probe: not a test; built with them and run by tools/load_test/load_run.py.
  add_executable(load_probe tools/load_test/load_probe.cpp)
  target_link_libraries(load_probe ${PROJECT_NAME}_core)
endif()

ament_package()
