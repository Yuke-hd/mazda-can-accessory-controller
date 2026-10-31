set(profile_runtime_sources
    "src/service.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_profile_runtime/src/extract.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_profile_runtime/src/provider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_profile_runtime/src/notifications.cpp")

if(COMMAND idf_component_register)
  idf_component_register(
    SRCS ${profile_runtime_sources}
    INCLUDE_DIRS
      "include"
      "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_profile_runtime/include"
    PRIV_INCLUDE_DIRS
      "${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_profile_runtime/src"
    REQUIRES vehicle_core vehicle_signals vehicle_telemetry
    PRIV_REQUIRES freertos
  )
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
  if(PROFILE_LINK_PROBE_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WEACT_PROFILE_LINK_PROBE=1)
  endif()
  return()
endif()

add_library(vehicle_profile_runtime_service STATIC src/service.cpp)
target_include_directories(vehicle_profile_runtime_service
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../lib/vehicle_profile_runtime/src)
target_link_libraries(vehicle_profile_runtime_service
  PUBLIC vehicle_profile_runtime vehicle_telemetry vehicle_core vehicle_signals)
target_compile_features(vehicle_profile_runtime_service PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(vehicle_profile_runtime_service PRIVATE /W4 /permissive-)
else()
  target_compile_options(vehicle_profile_runtime_service PRIVATE -Wall -Wextra -Wpedantic)
endif()
