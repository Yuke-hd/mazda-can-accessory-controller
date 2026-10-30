cmake_minimum_required(VERSION 3.16)

# This is the sole accessory-controller firmware configuration. Generic CAN,
# frame, and runtime components are supplied by the pinned vehicle-core
# dependency; Mazda adaptation remains in components/mazda_telemetry.

# R6-only compile/link probe. The default keeps production composition intact;
# selected synthetic profiles generate headers under the external build tree.
set(WEACT_CAN_PROFILE_LINK_PROBE "off" CACHE STRING
    "Retain one synthetic vehicle-profile runtime for resource measurement")
set_property(CACHE WEACT_CAN_PROFILE_LINK_PROBE PROPERTY STRINGS
             off empty populated max)
if(NOT WEACT_CAN_PROFILE_LINK_PROBE MATCHES "^(off|empty|populated|max)$")
  message(FATAL_ERROR
    "WEACT_CAN_PROFILE_LINK_PROBE must be off, empty, populated, or max")
endif()

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

if(NOT WEACT_CAN_PROFILE_LINK_PROBE STREQUAL "off")
  list(APPEND EXTRA_COMPONENT_DIRS
       "${CMAKE_CURRENT_LIST_DIR}/../../components/vehicle_profile_runtime")
endif()

set(VEHICLE_CAN_CORE_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp32-vehicle-can-core"
    CACHE PATH "Local vehicle CAN core checkout for offline firmware builds")
if(EXISTS "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_core")
  list(APPEND EXTRA_COMPONENT_DIRS
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_core"
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/can_bus"
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_telemetry")
endif()

# Generate the output catalog and limits header from the sink descriptors
# selected by this board's output_catalog.cmake. A malformed descriptor fails
# configuration; the build rule regenerates when an input changes.
include("${CMAKE_CURRENT_LIST_DIR}/../../tools/output_catalog.cmake")
output_catalog_generate(
  BOARD_FILE "${CMAKE_CURRENT_LIST_DIR}/output_catalog.cmake"
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/output_catalog")

# Compile the repository's authoring document before ESP-IDF discovers the
# controller_config component. The configure-time invocation makes malformed
# YAML fail the firmware configure immediately; the matching build rule keeps
# the embedded artifact current when the source document changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(NOT WEACT_CAN_PROFILE_LINK_PROBE STREQUAL "off")
  set(PROFILE_LINK_PROBE_GENERATED_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/vehicle_profile_link_probe/${WEACT_CAN_PROFILE_LINK_PROBE}")
  file(GLOB _profile_runtime_codegen_sources CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_LIST_DIR}/../../tools/vehicle_profiles/*.py")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_LIST_DIR}/../../tools/generate_profile_runtime_link_probe.py"
       "${CMAKE_CURRENT_LIST_DIR}/../../tests/fixtures/vehicle-profiles/synthetic-empty.json"
       "${CMAKE_CURRENT_LIST_DIR}/../../tests/fixtures/vehicle-profiles/synthetic-river.json"
       "${CMAKE_CURRENT_LIST_DIR}/../../tests/fixtures/vehicle-profiles/runtime-limits.json"
       ${_profile_runtime_codegen_sources})
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../../tools/generate_profile_runtime_link_probe.py"
            --selection "${WEACT_CAN_PROFILE_LINK_PROBE}"
            --output-dir "${PROFILE_LINK_PROBE_GENERATED_DIR}"
    RESULT_VARIABLE _profile_link_probe_result
    OUTPUT_VARIABLE _profile_link_probe_output
    ERROR_VARIABLE _profile_link_probe_error)
  if(NOT _profile_link_probe_result EQUAL 0)
    message(FATAL_ERROR
      "Synthetic profile link-probe generation failed:\n"
      "${_profile_link_probe_output}${_profile_link_probe_error}")
  endif()
endif()
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
    "${OUTPUT_CATALOG_JSON}")
message(STATUS "Factory controller configuration YAML: ${CONTROLLER_CONFIG_DEFAULT_YAML}")

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CONTROLLER_CONFIG_DEFAULT_YAML}" "${CONTROLLER_CONFIG_FACTORY_JSON}"
          --output-catalog "${OUTPUT_CATALOG_JSON}"
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
          --output-catalog "${OUTPUT_CATALOG_JSON}"
  DEPENDS "${CONTROLLER_CONFIG_DEFAULT_YAML}"
          "${CMAKE_CURRENT_LIST_DIR}/../../tools/compile_controller_config.py"
          "${CMAKE_CURRENT_LIST_DIR}/../../tools/controller_signal_catalog.json"
          "${OUTPUT_CATALOG_JSON}"
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
# This -O2 comes after the menuconfig optimisation level and overrides it for
# these components. To debug them at -Og or -O0 (for example under GDB),
# remove -O2 here temporarily; do not ship or vehicle-test such a build.
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
if(NOT WEACT_CAN_PROFILE_LINK_PROBE STREQUAL "off")
  # The anchor is test-only and is not called by app_main. Force extraction so
  # generated function pointers and their runtime caller graph survive gc-sections.
  target_link_options(${_project_elf} PRIVATE "-Wl,-u,vehicle_profile_link_probe_anchor")
endif()
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

# The flash-cache layout assumes 80 MHz DIO flash. A stale sdkconfig would keep
# the slower 40 MHz default without any other symptom.
if(NOT CONFIG_ESPTOOLPY_FLASHFREQ_80M OR NOT CONFIG_ESPTOOLPY_FLASHMODE_DIO)
  message(FATAL_ERROR
    "sdkconfig does not select 80 MHz DIO flash. Run 'idf.py set-target esp32' "
    "or delete sdkconfig to regenerate it from sdkconfig.defaults (see "
    "docs/development/flash-cache-layout.md).")
endif()
