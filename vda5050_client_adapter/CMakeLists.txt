cmake_minimum_required(VERSION 3.14)
project(vda5050_client_adapter)

# ─── Compiler settings ────────────────────────────────────────────────────────
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── ROS2 / ament dependencies ────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(vda5050_msgs REQUIRED)

# ─── nlohmann/json ────────────────────────────────────────────────────────────
# Prefer a system package first so builds work in offline or sandboxed setups.
find_package(nlohmann_json QUIET)

if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# ─── Paho MQTT C++ ────────────────────────────────────────────────────────────
# Install on Ubuntu/Debian:
#   sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev

set(PAHO_PREFIX "" CACHE PATH "Optional custom prefix where Paho MQTT is installed")

set(_paho_lib_hints
  ${PAHO_PREFIX}/lib
  ${PAHO_PREFIX}/lib64
  /usr/lib/x86_64-linux-gnu
  /usr/lib/aarch64-linux-gnu
  /usr/lib
  /usr/local/lib
)
set(_paho_inc_hints
  ${PAHO_PREFIX}/include
  /usr/include
  /usr/local/include
)

find_library(PAHO_MQTTPP_LIB
  NAMES paho-mqttpp3 HINTS ${_paho_lib_hints}NO_DEFAULT_PATH)
find_library(PAHO_MQTT_C_LIB
  NAMES paho-mqtt3as paho-mqtt3a HINTS ${_paho_lib_hints} NO_DEFAULT_PATH)
find_path(PAHO_MQTTPP_INCLUDE
  NAMES mqtt/async_client.h HINTS ${_paho_inc_hints} NO_DEFAULT_PATH)

if(PAHO_MQTTPP_LIB AND PAHO_MQTT_C_LIB AND PAHO_MQTTPP_INCLUDE)
  message(STATUS "[vda5050_client_adapter] Found Paho MQTT C++  : ${PAHO_MQTTPP_LIB}")
  message(STATUS "[vda5050_client_adapter] Found Paho MQTT C    : ${PAHO_MQTT_C_LIB}")
  message(STATUS "[vda5050_client_adapter] Found Paho include   : ${PAHO_MQTTPP_INCLUDE}")
  add_library(PahoMqttCpp::PahoMqttCpp UNKNOWN IMPORTED GLOBAL)
  set_target_properties(PahoMqttCpp::PahoMqttCpp PROPERTIES
    IMPORTED_LOCATION             "${PAHO_MQTTPP_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${PAHO_MQTTPP_INCLUDE}"
    INTERFACE_LINK_LIBRARIES      "${PAHO_MQTT_C_LIB}")
else()
  message(FATAL_ERROR
    "\n"
    "╔══════════════════════════════════════════════════════════╗\n"
    "║  [vda5050_client_adapter]  Paho MQTT C++ library NOT FOUND      ║\n"
    "╠══════════════════════════════════════════════════════════╣\n"
    "║  Install with:                                           ║\n"
    "║    sudo apt install libpaho-mqttpp-dev libpaho-mqtt-dev  ║\n"
    "║                                                          ║\n"
    "║  Detected:                                               ║\n"
    "║    PAHO_MQTTPP_LIB     = ${PAHO_MQTTPP_LIB}\n"
    "║    PAHO_MQTT_C_LIB     = ${PAHO_MQTT_C_LIB}\n"
    "║    PAHO_MQTTPP_INCLUDE = ${PAHO_MQTTPP_INCLUDE}\n"
    "╚══════════════════════════════════════════════════════════╝\n")
endif()

set(PAHO_MQTT_CPP_TARGET PahoMqttCpp::PahoMqttCpp)

# ─── Library: vda5050_adapter_core ────────────────────────────────────────────

add_library(${PROJECT_NAME}_core SHARED
  src/adapter_state_machine.cpp
  src/mqtt_client.cpp
  src/order_manager.cpp
  src/action_manager.cpp
  src/vda5050_node.cpp
)

target_include_directories(${PROJECT_NAME}_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

ament_target_dependencies(${PROJECT_NAME}_core
  rclcpp
  std_msgs
  vda5050_msgs
)

target_link_libraries(${PROJECT_NAME}_core
  ${PAHO_MQTT_CPP_TARGET}
  nlohmann_json::nlohmann_json
)

# Expose nlohmann headers at build-time via the include dir from FetchContent.
get_target_property(_nlohmann_inc nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(${PROJECT_NAME}_core PUBLIC
  $<BUILD_INTERFACE:${_nlohmann_inc}>
)

# ─── Executable ───────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_node src/main.cpp)

ament_target_dependencies(${PROJECT_NAME}_node rclcpp)
target_link_libraries(${PROJECT_NAME}_node ${PROJECT_NAME}_core)

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS
  ${PROJECT_NAME}_core
  ${PROJECT_NAME}_node
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config/  DESTINATION share/${PROJECT_NAME}/config)
install(DIRECTORY launch/  DESTINATION share/${PROJECT_NAME}/launch OPTIONAL)

# ─── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_order_manager
    test/test_order_manager.cpp)
  target_link_libraries(test_order_manager ${PROJECT_NAME}_core)
  ament_target_dependencies(test_order_manager
    rclcpp
    std_msgs
    vda5050_msgs
  )

  ament_add_gtest(test_action_manager
    test/test_action_manager.cpp)
  target_link_libraries(test_action_manager ${PROJECT_NAME}_core)
  ament_target_dependencies(test_action_manager
    rclcpp
    std_msgs
    vda5050_msgs
  )

  ament_add_gtest(test_converters
    test/test_converters.cpp)
  target_link_libraries(test_converters ${PROJECT_NAME}_core)
  ament_target_dependencies(test_converters
    rclcpp
    std_msgs
    vda5050_msgs
  )

  ament_add_gtest(test_adapter_state_machine
    test/test_adapter_state_machine.cpp)
  target_link_libraries(test_adapter_state_machine ${PROJECT_NAME}_core)
endif()

ament_export_dependencies(rclcpp std_msgs vda5050_msgs)
ament_package()
