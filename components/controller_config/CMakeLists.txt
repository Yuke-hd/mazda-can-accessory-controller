# Controller configuration: the application's configured features, expressed
# as generic engine rules and LED action bindings. It owns the feature choices,
# such as which signal drives which effect and over which input range, so
# neither the action engine nor the LED components learn about a signal. Only
# the firmware composition root applies it.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/lighting_profile_application.cpp" "src/rpm_level_fill.cpp"
                              "src/rpm_threshold.cpp" "src/schema.cpp" "src/json_loader.cpp"
                         INCLUDE_DIRS "include"
                         REQUIRES action_engine local_argb_actions vehicle_core
                         PRIV_REQUIRES json)
  return()
endif()

# Host tests use the same ESP-IDF-supported cJSON implementation as firmware.
# Prefer an ESP-IDF checkout when available, allow an explicit offline source,
# and otherwise import the pinned upstream release through FetchContent.
set(CONTROLLER_CONFIG_CJSON_SOURCE_DIR "" CACHE PATH
    "Path to a cJSON source checkout for host builds")
set(_controller_config_cjson_target "")
if(CONTROLLER_CONFIG_CJSON_SOURCE_DIR AND
   EXISTS "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}/cJSON.c" AND
   EXISTS "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}/cJSON.h")
  add_library(controller_config_cjson STATIC
    "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}/cJSON.c")
  target_include_directories(controller_config_cjson PUBLIC
    "${CONTROLLER_CONFIG_CJSON_SOURCE_DIR}")
  set(_controller_config_cjson_target controller_config_cjson)
elseif(DEFINED ENV{IDF_PATH} AND
       EXISTS "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
  add_library(controller_config_cjson STATIC
    "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
  target_include_directories(controller_config_cjson PUBLIC
    "$ENV{IDF_PATH}/components/json/cJSON")
  set(_controller_config_cjson_target controller_config_cjson)
else()
  include(FetchContent)
  FetchContent_Declare(controller_config_cjson_source
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.19
    GIT_SHALLOW TRUE)
  # cJSON v1.7.19 predates CMake 4's policy floor and does not publish its
  # source directory as a target include path. Keep those compatibility
  # details at the import boundary instead of exposing them to consumers.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  FetchContent_MakeAvailable(controller_config_cjson_source)
  if(TARGET cjson)
    target_include_directories(cjson PUBLIC
      "$<BUILD_INTERFACE:${controller_config_cjson_source_SOURCE_DIR}>")
    set(_controller_config_cjson_target cjson)
  else()
    message(FATAL_ERROR "cJSON v1.7.19 did not provide the expected cjson target")
  endif()
endif()

add_library(controller_config STATIC
  src/lighting_profile_application.cpp
  src/rpm_level_fill.cpp
  src/rpm_threshold.cpp
  src/schema.cpp
  src/json_loader.cpp
)
target_include_directories(controller_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(controller_config PUBLIC action_engine local_argb_actions vehicle_core)
target_link_libraries(controller_config PRIVATE ${_controller_config_cjson_target})
target_compile_features(controller_config PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(controller_config PRIVATE /W4 /permissive-)
else()
  target_compile_options(controller_config PRIVATE -Wall -Wextra -Wpedantic)
endif()
