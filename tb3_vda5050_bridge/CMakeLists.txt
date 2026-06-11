cmake_minimum_required(VERSION 3.8)
project(tb3_vda5050_bridge)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
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
  src/bridge_state_machine.cpp
  src/order_session.cpp
)

target_include_directories(${PROJECT_NAME}_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(${PROJECT_NAME}_core PUBLIC cxx_std_17)
ament_target_dependencies(${PROJECT_NAME}_core vda5050_msgs)

# ── Bridge node ───────────────────────────────────────────────────────────────
add_executable(tb3_vda5050_bridge_node
  src/bridge_node.cpp
)

target_include_directories(tb3_vda5050_bridge_node PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(tb3_vda5050_bridge_node PUBLIC cxx_std_17)
ament_target_dependencies(tb3_vda5050_bridge_node ${DEPS})
target_link_libraries(tb3_vda5050_bridge_node ${PROJECT_NAME}_core)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS
  ${PROJECT_NAME}_core
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

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_bridge_state_machine
    test/test_bridge_state_machine.cpp)
  target_link_libraries(test_bridge_state_machine ${PROJECT_NAME}_core)
  target_include_directories(test_bridge_state_machine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

  ament_add_gtest(test_order_session
    test/test_order_session.cpp)
  target_link_libraries(test_order_session ${PROJECT_NAME}_core)
  ament_target_dependencies(test_order_session vda5050_msgs)
  target_include_directories(test_order_session PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
endif()

ament_package()
