cmake_minimum_required(VERSION 3.16)

# This is the sole accessory-controller firmware configuration. Generic CAN,
# frame, and runtime components are supplied by the pinned vehicle-core
# dependency; Mazda adaptation remains in components/mazda_telemetry.

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/board"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/companion_ble"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/companion_config"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_sink_contract"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_actions"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/controller_config"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/vehicle_can_rx"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/mazda_telemetry"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/action_engine"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/companion_protocol"
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
set(_controller_config_default_yaml
    "${CMAKE_CURRENT_LIST_DIR}/../../config/default.yaml")
set(CONTROLLER_CONFIG_DEFAULT_YAML "${_controller_config_default_yaml}"
    CACHE FILEPATH "Factory controller configuration YAML")
set(CONTROLLER_CONFIG_FACTORY_JSON
    "${CMAKE_CURRENT_BINARY_DIR}/factory_default_config.json"
    CACHE FILEPATH "Generated factory controller configuration JSON" FORCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CONTROLLER_CONFIG_DEFAULT_YAML}"
    "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
    "${CMAKE_CURRENT_LIST_DIR}/../../tools/controller_signal_catalog.json"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_sink_contract/include/local_argb/lighting_sink.hpp")
message(STATUS "Factory controller configuration YAML: ${CONTROLLER_CONFIG_DEFAULT_YAML}")

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
file(SHA256 "${CONTROLLER_CONFIG_FACTORY_JSON}" _controller_config_factory_sha256)
message(STATUS "Factory controller configuration JSON SHA256: ${_controller_config_factory_sha256}")

add_custom_command(
  OUTPUT "${CONTROLLER_CONFIG_FACTORY_JSON}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CONTROLLER_CONFIG_DEFAULT_YAML}" "${CONTROLLER_CONFIG_FACTORY_JSON}"
  DEPENDS "${CONTROLLER_CONFIG_DEFAULT_YAML}"
          "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CMAKE_CURRENT_LIST_DIR}/../../tools/controller_signal_catalog.json"
          "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_sink_contract/include/local_argb/lighting_sink.hpp"
  COMMENT "Compiling factory controller configuration")
add_custom_target(controller_config_factory_json
  DEPENDS "${CONTROLLER_CONFIG_FACTORY_JSON}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(weact_can485_v11_vehicle_listen_only)

# Flash-cache layout (docs/development/flash-cache-layout.md). The ESP32's
# shared flash cache made telemetry throughput depend on how unrelated Kconfig
# options shifted the flash layout. Compile the project's own components with
# -O2; main/hot_rodata.lf and main/hot_code.lf move their rodata to DRAM and
# the hot telemetry functions to IRAM. These options apply to this firmware
# target only; host builds of lib/ and tests are unaffected.
foreach(_component mazda_telemetry mazda vehicle_telemetry vehicle_core vehicle_signals
                   action_engine vehicle_lighting_policy vehicle_can_rx can_bus local_argb
                   local_argb_actions)
  idf_component_get_property(_component_lib ${_component} COMPONENT_LIB)
  get_target_property(_component_type ${_component_lib} TYPE)
  if(NOT _component_type STREQUAL "INTERFACE_LIBRARY")
    target_compile_options(${_component_lib} PRIVATE -O2)
  endif()
endforeach()
# ldgen symbol entries cannot contain the `$` of GCC clone suffixes, so keep
# the hot_code.lf functions clone-free.
idf_component_get_property(_mazda_telemetry_lib mazda_telemetry COMPONENT_LIB)
target_compile_options(${_mazda_telemetry_lib} PRIVATE
  -fno-ipa-sra -fno-ipa-cp-clone -fno-partial-inlining -fno-ipa-cp)

# ldgen silently ignores a symbol entry that matches nothing. Fail the build
# when any hot_code.lf symbol is missing from the ELF or outside IRAM.
idf_build_get_property(_idf_python PYTHON)
idf_build_get_property(_project_elf EXECUTABLE)
add_custom_command(TARGET ${_project_elf} POST_BUILD
  COMMAND "${_idf_python}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/check_iram_placement.py"
          --fragment "${CMAKE_CURRENT_LIST_DIR}/main/hot_code.lf"
          --elf "$<TARGET_FILE:${_project_elf}>"
          --nm "${CMAKE_NM}" --objdump "${CMAKE_OBJDUMP}"
  COMMENT "Checking IRAM placement of hot_code.lf symbols"
  VERBATIM)

# sdkconfig.defaults does not override an existing sdkconfig. Refuse to build
# with a stale configuration that would flash the old 24 KiB NVS table and
# strand entries written to the larger partition.
if(NOT CONFIG_PARTITION_TABLE_CUSTOM
   OR NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions.csv")
  message(FATAL_ERROR
    "sdkconfig does not select the custom partitions.csv table. Run "
    "'idf.py set-target esp32' or delete sdkconfig to regenerate it from "
    "sdkconfig.defaults (see docs/development/firmware-build.md).")
endif()
