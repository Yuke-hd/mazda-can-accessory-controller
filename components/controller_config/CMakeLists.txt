# Controller configuration: the application's configured features, expressed
# as generic engine rules and LED action bindings. It owns the feature choices,
# such as which signal drives which effect and over which input range, so
# neither the action engine nor the LED components learn about a signal. Only
# the firmware composition root applies it.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/lighting_profile_application.cpp" "src/rpm_level_fill.cpp"
                              "src/rpm_threshold.cpp" "src/persisted/names.cpp"
                              "src/persisted/production_profile.cpp"
                              "src/persisted/validation.cpp" "src/persisted/json_loader.cpp"
                         INCLUDE_DIRS "include"
                         REQUIRES action_engine local_argb_actions vehicle_core
                         PRIV_REQUIRES json)
  return()
endif()

# Host tests use the same ESP-IDF-supported cJSON implementation as firmware.
# An explicit source checkout is supported for offline builds; otherwise the
# pinned upstream release is fetched. The upstream CMake project is not added
# to this build: only cJSON.c is compiled into a private static target, so its
# options, tests, and shared-library targets cannot leak into this project.
set(CONTROLLER_CONFIG_CJSON_SOURCE_DIR "" CACHE PATH
    "Path to a cJSON source checkout for host builds")
if(CONTROLLER_CONFIG_CJSON_SOURCE_DIR)
  if(NOT EXISTS "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}/cJSON.c" OR
     NOT EXISTS "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}/cJSON.h")
    message(FATAL_ERROR
      "CONTROLLER_CONFIG_CJSON_SOURCE_DIR must contain cJSON.c and cJSON.h")
  endif()
  set(_controller_config_cjson_source_dir
      "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}")
else()
  include(FetchContent)
  FetchContent_Declare(controller_config_cjson_source
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    # v1.7.19 release commit; use the immutable SHA rather than a movable tag.
    GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3
    GIT_SHALLOW FALSE
    # Populate the source without adding cJSON's own CMake project.
    SOURCE_SUBDIR cmake-import-disabled)
  FetchContent_MakeAvailable(controller_config_cjson_source)
  set(_controller_config_cjson_source_dir
      "${controller_config_cjson_source_SOURCE_DIR}")
endif()

add_library(controller_config_cjson STATIC EXCLUDE_FROM_ALL
  "${_controller_config_cjson_source_dir}/cJSON.c")
target_include_directories(controller_config_cjson PRIVATE
  "${_controller_config_cjson_source_dir}")

add_library(controller_config STATIC
  src/lighting_profile_application.cpp
  src/rpm_level_fill.cpp
  src/rpm_threshold.cpp
  src/persisted/names.cpp
  src/persisted/production_profile.cpp
  src/persisted/validation.cpp
  src/persisted/json_loader.cpp
)
target_include_directories(controller_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(controller_config PRIVATE "${_controller_config_cjson_source_dir}")
target_link_libraries(controller_config PRIVATE controller_config_cjson)
target_link_libraries(controller_config PUBLIC action_engine local_argb_actions vehicle_core)
target_compile_features(controller_config PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(controller_config PRIVATE /W4 /permissive-)
else()
  target_compile_options(controller_config PRIVATE -Wall -Wextra -Wpedantic)
endif()
