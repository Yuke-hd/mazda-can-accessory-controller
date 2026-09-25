# Test support is intentionally header-only. Keep fake time and direct frame
# injection outside product components so test seams cannot become firmware
# dependencies.
add_library(telemetry_test_support INTERFACE)
target_include_directories(telemetry_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(telemetry_test_support INTERFACE vehicle_core)

# Generic provider fake over a caller-supplied catalog. It links only the
# portable signal contracts, so engine and consumer tests stay make-free.
add_library(vehicle_signals_test_support INTERFACE)
target_include_directories(vehicle_signals_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vehicle_signals_test_support INTERFACE vehicle_signals)

# Recording action sink for engine and composition tests.
add_library(action_engine_test_support INTERFACE)
target_include_directories(action_engine_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(action_engine_test_support INTERFACE action_engine)
