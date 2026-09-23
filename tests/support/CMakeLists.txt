# Test support is intentionally header-only. Keep fake time and direct frame
# injection outside product components so test seams cannot become firmware
# dependencies.
add_library(telemetry_test_support INTERFACE)
target_include_directories(telemetry_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(telemetry_test_support INTERFACE vehicle_core)
