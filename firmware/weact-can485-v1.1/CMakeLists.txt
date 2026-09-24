cmake_minimum_required(VERSION 3.16)

# This is the sole accessory-controller firmware configuration. Generic CAN,
# frame, and runtime components are supplied by the pinned vehicle-core
# dependency; Mazda adaptation remains in components/mazda_telemetry.

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/board"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb_sink_contract"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/local_argb"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/vehicle_can_rx"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_signals"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/mazda_telemetry"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/mazda"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_lighting_policy")

set(VEHICLE_CAN_CORE_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp32-vehicle-can-core"
    CACHE PATH "Local vehicle CAN core checkout for offline firmware builds")
if(EXISTS "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_core")
  list(APPEND EXTRA_COMPONENT_DIRS
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_core"
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/can_bus"
       "${VEHICLE_CAN_CORE_SOURCE_DIR}/components/vehicle_telemetry")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(weact_can485_v11_vehicle_listen_only)
