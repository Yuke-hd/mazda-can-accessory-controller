# Portable, header-only signal contracts and catalog view. The only dependency
# is the companion core's value-only telemetry contracts; no Mazda, CAN, RTOS,
# or driver component is reachable from this target.
if(COMMAND idf_component_register)
  idf_component_register(INCLUDE_DIRS "include" REQUIRES vehicle_core)
  return()
endif()

add_library(vehicle_signals INTERFACE)
target_include_directories(vehicle_signals INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vehicle_signals INTERFACE vehicle_core)
target_compile_features(vehicle_signals INTERFACE cxx_std_17)
