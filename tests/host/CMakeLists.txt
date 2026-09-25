include(FetchContent)

# Keep the host test dependency reproducible. Upgrade only in a dedicated change.
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  # v2.5.0 raises doctest's CMake minimum to 3.14, which remains compatible
  # with the project's 3.20 minimum and modern CMake 4.x.
  GIT_TAG d44d4f6e66232d716af82f00a063759e9d0e50d6
)
FetchContent_MakeAvailable(doctest)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

add_executable(telemetry_contract_compile_tests telemetry_contract_compile_tests.cpp)
target_link_libraries(telemetry_contract_compile_tests PRIVATE mazda_telemetry_contracts
                                                             telemetry_test_support
                                                             doctest::doctest)
target_compile_features(telemetry_contract_compile_tests PRIVATE cxx_std_17)
doctest_discover_tests(telemetry_contract_compile_tests)

add_executable(telemetry_internal_contract_tests telemetry_internal_contract_tests.cpp)
target_link_libraries(telemetry_internal_contract_tests PRIVATE doctest::doctest)
target_include_directories(telemetry_internal_contract_tests PRIVATE
  ${MAZDA_VEHICLE_CAN_CORE_ROOT}/components/vehicle_core/include
)
target_compile_features(telemetry_internal_contract_tests PRIVATE cxx_std_17)
target_link_libraries(telemetry_internal_contract_tests PRIVATE local_argb_sink_contract)
doctest_discover_tests(telemetry_internal_contract_tests)

add_executable(mazda_candidate_tests mazda_candidate_tests.cpp)
target_link_libraries(mazda_candidate_tests PRIVATE mazda telemetry_test_support doctest::doctest)
target_compile_features(mazda_candidate_tests PRIVATE cxx_std_17)
doctest_discover_tests(mazda_candidate_tests)

add_executable(mazda_turn_tests mazda_turn_tests.cpp)
target_link_libraries(mazda_turn_tests PRIVATE mazda telemetry_test_support doctest::doctest)
target_compile_features(mazda_turn_tests PRIVATE cxx_std_17)
doctest_discover_tests(mazda_turn_tests)

add_executable(mazda_state_tests mazda_state_tests.cpp)
target_link_libraries(mazda_state_tests PRIVATE mazda telemetry_test_support doctest::doctest)
target_compile_features(mazda_state_tests PRIVATE cxx_std_17)
doctest_discover_tests(mazda_state_tests)

add_executable(mazda_availability_tests mazda_availability_tests.cpp)
target_link_libraries(mazda_availability_tests PRIVATE mazda telemetry_test_support doctest::doctest)
target_compile_features(mazda_availability_tests PRIVATE cxx_std_17)
doctest_discover_tests(mazda_availability_tests)

# Portable signal contracts link only the generic target and doctest; no Mazda
# target or test support is reachable from this test.
add_executable(vehicle_signals_tests vehicle_signals_tests.cpp)
target_link_libraries(vehicle_signals_tests PRIVATE vehicle_signals doctest::doctest)
target_compile_features(vehicle_signals_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(vehicle_signals_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(vehicle_signals_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(vehicle_signals_tests)

# The generic consumer sees only the public provider/facade interface: it has
# no private include path, and architecture_contracts restricts its includes
# to mazda/signal_provider.hpp and vehicle_signals headers.
add_library(generic_signal_consumer STATIC generic_signal_consumer.cpp)
target_link_libraries(generic_signal_consumer PUBLIC mazda_telemetry_contracts)
target_compile_features(generic_signal_consumer PUBLIC cxx_std_17)

# The test harness alone uses the private host-service seam to drive generated
# frames through the production runtime, Mazda decoder and publication path.
add_executable(signal_consumer_tests signal_consumer_tests.cpp)
target_include_directories(signal_consumer_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(signal_consumer_tests PRIVATE generic_signal_consumer mazda_telemetry
                                                    doctest::doctest)
target_compile_features(signal_consumer_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(generic_signal_consumer PRIVATE /W4 /permissive-)
  target_compile_options(signal_consumer_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(generic_signal_consumer PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(signal_consumer_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(signal_consumer_tests)

add_executable(board_config_tests board_config_tests.cpp)
target_include_directories(board_config_tests PRIVATE ${CMAKE_SOURCE_DIR}/components/board/include)
target_compile_features(board_config_tests PRIVATE cxx_std_17)
add_test(NAME board_config_tests COMMAND board_config_tests)

add_executable(
  local_argb_tests
  local_argb_tests.cpp
  ${CMAKE_SOURCE_DIR}/firmware/weact-can485-v1.1/main/semantic_led_policy.cpp
)
target_include_directories(
  local_argb_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/components/local_argb/include
    ${CMAKE_SOURCE_DIR}/firmware/weact-can485-v1.1/main
    ${MAZDA_VEHICLE_CAN_CORE_ROOT}/components/vehicle_core/include
)
target_link_libraries(
  local_argb_tests
  PRIVATE local_argb_firmware_compat doctest::doctest
)
target_compile_features(local_argb_tests PRIVATE cxx_std_17)
doctest_discover_tests(local_argb_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME architecture_contracts
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/check_architecture.py
          --root ${CMAKE_SOURCE_DIR}
          --core-root ${MAZDA_VEHICLE_CAN_CORE_ROOT}
          --compiler ${CMAKE_CXX_COMPILER}
          --cmake ${CMAKE_COMMAND}
)

# Required Stage 1.5 gate. The checker compiles each public entry point in
# isolation, verifies the ordinary consumer target's exported include paths,
# and keeps explicit internal access separate from public-header coverage.
# Keep this as the single CTest registration; the host CI jobs invoke CTest
# once per configured build and must not run the checker as a second step.
add_test(
  NAME public_header_boundary
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/check_public_headers.py
          --root ${CMAKE_SOURCE_DIR}
          --core-root ${MAZDA_VEHICLE_CAN_CORE_ROOT}
          --compiler ${CMAKE_CXX_COMPILER}
          --cmake ${CMAKE_COMMAND}
)

add_test(
  NAME public_header_checker_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tests/header_boundary/check_public_headers_test.py
          --compiler ${CMAKE_CXX_COMPILER}
          --cmake ${CMAKE_COMMAND}
)

add_test(
  NAME architecture_checker_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tests/tools/check_architecture_test.py
)
