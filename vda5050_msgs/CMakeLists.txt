cmake_minimum_required(VERSION 3.8)
project(vda5050_msgs)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  # Primitive/shared types (no dependencies)
  "msg/ActionParameter.msg"
  "msg/BoundingBoxReference.msg"
  "msg/ControlPoint.msg"
  "msg/LoadDimensions.msg"
  "msg/ErrorReference.msg"
  "msg/InfoReference.msg"
  "msg/SafetyState.msg"
  "msg/Velocity.msg"

  # Types depending only on primitives above
  "msg/Action.msg"
  "msg/Trajectory.msg"
  "msg/Corridor.msg"
  "msg/NodePosition.msg"
  "msg/Error.msg"
  "msg/Info.msg"
  "msg/AgvPosition.msg"
  "msg/BatteryState.msg"
  "msg/ActionState.msg"
  "msg/MapInfo.msg"

  # Types depending on the above
  "msg/Load.msg"
  "msg/Node.msg"
  "msg/Edge.msg"
  "msg/NodeState.msg"
  "msg/EdgeState.msg"

  # Top-level message types
  "msg/Header.msg"
  "msg/Order.msg"
  "msg/InstantActions.msg"
  "msg/State.msg"
  "msg/Connection.msg"
  "msg/Visualization.msg"
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
