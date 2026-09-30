cmake_minimum_required(VERSION 3.16)

# This is the sole accessory-controller firmware configuration. Generic CAN,
# frame, and runtime components are supplied by the pinned vehicle-core
# dependency; Mazda adaptation remains in components/mazda_telemetry.

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/board"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_sink_contract"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_actions"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller_config"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/vehicle_can_rx"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/mazda_telemetry"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/action_engine"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/mazda"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_lighting_policy"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_signals")

set(VEHICLE_CAN_CORE_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp32-vehicle-can-core"
    CACHE PATH "Local vehicle CAN core checkout for offline firmware builds")
if(EXISTS "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_core")
  list(APPEND EXTRA_COMPONENT_DIRS
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_core"
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/can_bus"
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_telemetry")
endif()

# Compile the repository's authoring document before ESP-IDF discovers the
# controller_config component. The configure-time invocation makes malformed
# YAML fail the firmware configure immediately; the matching build rule keeps
# the embedded artifact current when the source document changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(CONTROLLER_CONFIG_DEFAULT_YAML
    "${CMAKE_CURRENT_LIST_DIR}/../../docs/examples/controller-config-v1.yaml"
    CACHE FILEPATH "Factory controller configuration YAML")
set(CONTROLLER_CONFIG_FACTORY_JSON
    "${CMAKE_CURRENT_BINARY_DIR}/factory_default_config.json"
    CACHE FILEPATH "Generated factory controller configuration JSON" FORCE)

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CONTROLLER_CONFIG_DEFAULT_YAML}" "${CONTROLLER_CONFIG_FACTORY_JSON}"
  RESULT_VARIABLE _controller_config_compile_result
  OUTPUT_VARIABLE _controller_config_compile_output
  ERROR_VARIABLE _controller_config_compile_error)
if(NOT _controller_config_compile_result EQUAL 0)
  message(FATAL_ERROR
    "Factory controller configuration compilation failed:\n"
    "${_controller_config_compile_output}${_controller_config_compile_error}")
endif()

add_custom_command(
  OUTPUT "${CONTROLLER_CONFIG_FACTORY_JSON}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CONTROLLER_CONFIG_DEFAULT_YAML}" "${CONTROLLER_CONFIG_FACTORY_JSON}"
  DEPENDS "${CONTROLLER_CONFIG_DEFAULT_YAML}"
          "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CMAKE_CURRENT_LIST_DIR}/../../tools/controller_signal_catalog.json"
  COMMENT "Compiling factory controller configuration")
add_custom_target(controller_config_factory_json
  DEPENDS "${CONTROLLER_CONFIG_FACTORY_JSON}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(weact_can485_v11_vehicle_listen_only)
