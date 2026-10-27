# CMake 3.20 is the supported host minimum. The policy range keeps modern
# CMake releases (including CMake 4.x) on the policies this project tested.
cmake_minimum_required(VERSION 3.20...4.4)

project(mazda_can_accessory_controller_host LANGUAGES C CXX)

include(CTest)

include(FetchContent)

# The generic CAN/telemetry runtime is maintained in the companion core
# repository. Pin this dependency to a reviewed release tag.
set(VEHICLE_CAN_CORE_REPOSITORY
    "https://github.com/Yuke-hd/esp32-vehicle-can-core.git"
    CACHE STRING "Pinned vehicle CAN core repository")
set(VEHICLE_CAN_CORE_TAG
    "0.2.1"
    CACHE STRING "Vehicle CAN core release tag")
set(VEHICLE_CAN_CORE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/esp32-vehicle-can-core"
    CACHE PATH "Local vehicle CAN core checkout for offline builds")

if(EXISTS "${VEHICLE_CAN_CORE_SOURCE_DIR}/CMakeLists.txt")
  # The companion repository's own host suite is run in its repository. Its
  # CMake uses CMAKE_SOURCE_DIR for test-only include probes, so do not import
  # those unrelated tests into this controller's build tree.
  set(VEHICLE_CAN_CORE_BUILD_HOST_TESTS OFF CACHE BOOL
      "Disable companion core tests when embedded in the controller" FORCE)
  set(_mazda_build_testing ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  add_subdirectory("${VEHICLE_CAN_CORE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/vehicle-can-core")
  set(BUILD_TESTING ${_mazda_build_testing})
  set(MAZDA_VEHICLE_CAN_CORE_ROOT "${VEHICLE_CAN_CORE_SOURCE_DIR}")
else()
  set(VEHICLE_CAN_CORE_BUILD_HOST_TESTS OFF CACHE BOOL
      "Disable companion core tests when embedded in the controller" FORCE)
  FetchContent_Declare(
    vehicle_can_core
    GIT_REPOSITORY "${VEHICLE_CAN_CORE_REPOSITORY}"
    GIT_TAG "${VEHICLE_CAN_CORE_TAG}"
    GIT_SHALLOW OFF
  )
  set(_mazda_build_testing ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  FetchContent_MakeAvailable(vehicle_can_core)
  set(BUILD_TESTING ${_mazda_build_testing})
  set(MAZDA_VEHICLE_CAN_CORE_ROOT "${vehicle_can_core_SOURCE_DIR}")
endif()
set(MAZDA_VEHICLE_CAN_CORE_ROOT "${MAZDA_VEHICLE_CAN_CORE_ROOT}" CACHE PATH
    "Resolved vehicle CAN core source used by host tests and validators" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MAZDA_BUILD_HOST_TESTS "Build Mazda accessory host tests" ${BUILD_TESTING})
option(MAZDA_ENABLE_DECODER_CALL_PROBING
       "Compile host-only decoder routing call-count probes" OFF)

# Output catalog and limits header for the WeAct board, generated from the
# sink descriptors. Host tests compile the same header as the firmware.
include(tools/output_catalog.cmake)
output_catalog_generate(
  BOARD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/firmware/weact-can485-v1.1/output_catalog.cmake"
  OUTPUT_DIR "${CMAKE_BINARY_DIR}/output_catalog")

add_subdirectory(lib/mazda)
add_subdirectory(lib/gvret)
add_subdirectory(lib/vehicle_lighting_policy)
add_subdirectory(components/local_argb_sink_contract)
add_subdirectory(components/local_argb)
add_subdirectory(lib/vehicle_signals)
add_subdirectory(lib/action_engine)
add_subdirectory(lib/companion_protocol)
add_subdirectory(components/local_argb_actions)
add_subdirectory(components/controller_config)
add_subdirectory(components/companion_config)
add_subdirectory(components/mazda_telemetry)
add_subdirectory(components/companion_ble)
add_subdirectory(lib/replay)

# Standalone host catalog export does not depend on test or factory-config
# targets, so it can bootstrap later compiler-manifest generation.
add_executable(export_controller_signal_catalog tools/export_controller_signal_catalog.cpp)
target_include_directories(export_controller_signal_catalog PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/mazda/internal_include)
target_link_libraries(export_controller_signal_catalog PRIVATE mazda vehicle_signals)
target_compile_features(export_controller_signal_catalog PRIVATE cxx_std_17)

# The checked-in manifest bootstraps firmware configuration. These host-only
# operations update/check it explicitly, without executing a target binary.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(_catalog_operation regenerate check)
  set(_catalog_arguments)
  if(_catalog_operation STREQUAL "check")
    list(APPEND _catalog_arguments --check)
  endif()
  add_custom_target(${_catalog_operation}_controller_signal_catalog
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_controller_signal_catalog.py"
            --exporter $<TARGET_FILE:export_controller_signal_catalog>
            ${_catalog_arguments}
    DEPENDS export_controller_signal_catalog
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_controller_signal_catalog.py"
    VERBATIM)
endforeach()

if(MAZDA_BUILD_HOST_TESTS)
  enable_testing()
  add_subdirectory(tests/support)
  add_subdirectory(tests/host)
endif()
