if(COMMAND idf_component_register)
  idf_component_register(
    SRCS "src/local_argb.cpp" "src/led_zone.cpp" "src/local_argb_idf.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES board vehicle_core esp_timer led_strip esp_driver_spi
    PRIV_REQUIRES freertos local_argb_sink_contract
  )
  return()
endif()

add_library(local_argb STATIC src/local_argb.cpp src/led_zone.cpp)
target_include_directories(
  local_argb
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
)
target_link_libraries(local_argb PUBLIC vehicle_core)
target_link_libraries(local_argb PRIVATE local_argb_sink_contract)
target_compile_features(local_argb PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(local_argb PRIVATE /W4 /permissive-)
else()
  target_compile_options(local_argb PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  add_executable(local_argb_rendering_tests tests/rendering_tests.cpp)
  target_include_directories(
    local_argb_rendering_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/private_include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vehicle_lighting_policy/include
  )
  target_link_libraries(local_argb_rendering_tests PRIVATE local_argb local_argb_sink_contract)
  target_compile_features(local_argb_rendering_tests PRIVATE cxx_std_17)
  add_test(NAME local_argb_rendering_tests COMMAND local_argb_rendering_tests)

  add_executable(local_argb_zone_tests tests/zone_tests.cpp)
  target_include_directories(
    local_argb_zone_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/private_include
  )
  target_link_libraries(local_argb_zone_tests PRIVATE local_argb local_argb_sink_contract)
  target_compile_features(local_argb_zone_tests PRIVATE cxx_std_17)
  add_test(NAME local_argb_zone_tests COMMAND local_argb_zone_tests)

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME local_argb_public_boundary
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_boundary.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/../..
            --core-root ${MAZDA_VEHICLE_CAN_CORE_ROOT}
            --compiler ${CMAKE_CXX_COMPILER}
  )
endif()
