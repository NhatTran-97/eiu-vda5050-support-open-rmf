cmake_minimum_required(VERSION 3.8)
project(tb3_vda5050_bridge)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(nav2_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)
find_package(vda5050_msgs REQUIRED)

set(DEPS
  rclcpp
  rclcpp_action
  diagnostic_msgs
  nav2_msgs
  nav_msgs
  sensor_msgs
  geometry_msgs
  std_msgs
  tf2
  tf2_geometry_msgs
  vda5050_msgs
)

# ── Core modules ──────────────────────────────────────────────────────────────
add_library(${PROJECT_NAME}_core
  src/odom_distance_tracker.cpp
)

target_include_directories(${PROJECT_NAME}_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(${PROJECT_NAME}_core PUBLIC cxx_std_17)
ament_target_dependencies(${PROJECT_NAME}_core vda5050_msgs)

# ── Bridge node ───────────────────────────────────────────────────────────────
add_library(${PROJECT_NAME}_node_lib
  src/bridge_node.cpp
)

target_include_directories(${PROJECT_NAME}_node_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(${PROJECT_NAME}_node_lib PUBLIC cxx_std_17)
ament_target_dependencies(${PROJECT_NAME}_node_lib ${DEPS})
target_link_libraries(${PROJECT_NAME}_node_lib ${PROJECT_NAME}_core)

add_executable(tb3_vda5050_bridge_node
  src/main.cpp
)

target_compile_features(tb3_vda5050_bridge_node PUBLIC cxx_std_17)
ament_target_dependencies(tb3_vda5050_bridge_node rclcpp)
target_link_libraries(tb3_vda5050_bridge_node ${PROJECT_NAME}_node_lib)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS
  ${PROJECT_NAME}_core
  ${PROJECT_NAME}_node_lib
  tb3_vda5050_bridge_node
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY config launch docs
  DESTINATION share/${PROJECT_NAME}
)

install(PROGRAMS
  mock/mock_load_publisher.py
  DESTINATION lib/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_odom_distance_tracker
    test/test_odom_distance_tracker.cpp)
  target_link_libraries(test_odom_distance_tracker ${PROJECT_NAME}_core)
  target_include_directories(test_odom_distance_tracker PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

  ament_add_gtest(test_bridge_node
    test/test_bridge_node.cpp
    TIMEOUT 300)
  target_link_libraries(test_bridge_node ${PROJECT_NAME}_node_lib)
  ament_target_dependencies(test_bridge_node ${DEPS})

  # Fleet adapter -> client adapter -> bridge -> fake Nav2, over MQTT (needs VDA5050_TEST_BROKER).
  find_package(vda5050_client_adapter QUIET)
  find_package(nlohmann_json QUIET)
  set(VDA5050_FULL_CONTROL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vda5050_fleet_adapter_full_control"
    CACHE PATH "Source tree of vda5050_fleet_adapter_full_control for test_system_e2e")
  set(_full_control_files "")
  set(_full_control_found TRUE)
  foreach(_source message_builder order_handler instant_action_handler json_read state_handler cancel_tracker)
    set(_file "${VDA5050_FULL_CONTROL_SOURCE_DIR}/src/vda5050/${_source}.cpp")
    if(NOT EXISTS "${_file}")
      set(_full_control_found FALSE)
    endif()
    list(APPEND _full_control_files "${_file}")
  endforeach()

  if(vda5050_client_adapter_FOUND AND nlohmann_json_FOUND AND _full_control_found)
    ament_add_gtest(test_system_e2e
      test/test_system_e2e.cpp
      ${_full_control_files}
      TIMEOUT 300)
    target_include_directories(test_system_e2e PRIVATE "${VDA5050_FULL_CONTROL_SOURCE_DIR}/include")
    target_link_libraries(test_system_e2e ${PROJECT_NAME}_node_lib nlohmann_json::nlohmann_json)
    ament_target_dependencies(test_system_e2e ${DEPS} vda5050_client_adapter)
  else()
    message(STATUS "[tb3_vda5050_bridge] vda5050_client_adapter or vda5050_fleet_adapter_full_control "
                   "sources not found -- test_system_e2e skipped")
  endif()
endif()

ament_package()
