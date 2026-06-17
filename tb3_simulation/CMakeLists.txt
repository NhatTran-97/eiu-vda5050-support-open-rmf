cmake_minimum_required(VERSION 3.8)
project(tb3_simulation)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(Python3 REQUIRED)

# ---------------------------------------------------------------------------
# building_map_generator (từ rmf_building_map_tools) để generate Gazebo world
# từ file .building.yaml của traffic-editor.
# ---------------------------------------------------------------------------
find_program(BUILDING_MAP_GENERATOR_EXECUTABLE building_map_generator
  HINTS
    "/opt/ros/jazzy/lib/rmf_building_map_tools"
)

if(NOT BUILDING_MAP_GENERATOR_EXECUTABLE)
  message(FATAL_ERROR
    "[tb3_simulation] building_map_generator not found! "
    "Cần cài ros-jazzy-rmf-building-map-tools và source /opt/ros/jazzy/setup.bash."
  )
endif()

message(STATUS "[tb3_simulation] Found building_map_generator: ${BUILDING_MAP_GENERATOR_EXECUTABLE}")

set(AMENT_PREFIX_PATH_VALUE "$ENV{AMENT_PREFIX_PATH}")

if(NOT AMENT_PREFIX_PATH_VALUE)
  message(FATAL_ERROR
    "[tb3_simulation] AMENT_PREFIX_PATH chưa set! "
    "Run: source /opt/ros/jazzy/setup.bash"
  )
endif()

set(PATCH_WORLD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_world.py)

file(GLOB_RECURSE building_yaml
  "${CMAKE_CURRENT_SOURCE_DIR}/maps/*.building.yaml"
)

if(NOT building_yaml)
  message(FATAL_ERROR "[tb3_simulation] Không tìm thấy file .building.yaml nào trong maps/")
endif()

foreach(path ${building_yaml})

  string(REGEX REPLACE "\\.building\\.yaml$" "" base ${path})
  string(REGEX MATCH "[^/]+$" world ${base})

  set(out_dir     ${CMAKE_CURRENT_BINARY_DIR}/maps/${world})
  set(world_file  ${out_dir}/${world}.world)
  set(model_dir   ${out_dir}/models)
  set(nav_dir     ${out_dir}/nav_graphs)
  set(world_phony ${out_dir}/world_phony)
  set(nav_phony   ${nav_dir}/nav_phony)

  # Generate Gazebo world + patch sensors plugin (lidar/imu)
  add_custom_command(
    OUTPUT ${world_phony}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${model_dir}
    COMMAND ${CMAKE_COMMAND} -E env
            AMENT_PREFIX_PATH=${AMENT_PREFIX_PATH_VALUE}
            ${BUILDING_MAP_GENERATOR_EXECUTABLE}
            gazebo ${path} ${world_file} ${model_dir}
    COMMAND ${Python3_EXECUTABLE} ${PATCH_WORLD_SCRIPT} ${world_file}
    COMMAND ${CMAKE_COMMAND} -E touch ${world_phony}
    DEPENDS ${path} ${PATCH_WORLD_SCRIPT}
    COMMENT "[tb3_simulation] Generating + patching Gazebo world: ${world}"
    VERBATIM
  )

  add_custom_target(generate_${world}_world ALL
    DEPENDS ${world_phony}
  )

  # Generate nav graphs
  add_custom_command(
    OUTPUT ${nav_phony}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${nav_dir}
    COMMAND ${CMAKE_COMMAND} -E env
            AMENT_PREFIX_PATH=${AMENT_PREFIX_PATH_VALUE}
            ${BUILDING_MAP_GENERATOR_EXECUTABLE}
            nav ${path} ${nav_dir}
    COMMAND ${CMAKE_COMMAND} -E touch ${nav_phony}
    DEPENDS ${path}
    COMMENT "[tb3_simulation] Generating nav graphs: ${world}"
    VERBATIM
  )

  add_custom_target(generate_${world}_nav ALL
    DEPENDS ${nav_phony}
  )

  install(
    DIRECTORY ${out_dir}
    DESTINATION share/${PROJECT_NAME}/maps
    OPTIONAL
  )

endforeach()

install(
  DIRECTORY maps/
  DESTINATION share/${PROJECT_NAME}/maps
)

install(
  DIRECTORY launch/
  DESTINATION share/${PROJECT_NAME}/launch
)

install(
  DIRECTORY config/
  DESTINATION share/${PROJECT_NAME}/config
)

install(
  DIRECTORY scripts/
  DESTINATION share/${PROJECT_NAME}/scripts
)

ament_package()
