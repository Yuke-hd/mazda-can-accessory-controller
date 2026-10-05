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

add_executable(gvret_parser_tests gvret_parser_tests.cpp)
target_link_libraries(gvret_parser_tests PRIVATE gvret_parser doctest::doctest)
target_compile_features(gvret_parser_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(gvret_parser_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(gvret_parser_tests)

add_executable(gvret_replay_tests gvret_replay_tests.cpp)
target_link_libraries(gvret_replay_tests PRIVATE gvret_parser doctest::doctest)
target_compile_features(gvret_replay_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(gvret_replay_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_replay_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(gvret_replay_tests)

add_executable(replay_clock_tests replay_clock_tests.cpp)
target_link_libraries(replay_clock_tests PRIVATE replay_clock doctest::doctest)
target_compile_features(replay_clock_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_clock_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_clock_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_clock_tests)

add_executable(replay_acquisition_source_tests replay_acquisition_source_tests.cpp)
target_link_libraries(replay_acquisition_source_tests PRIVATE replay_source doctest::doctest)
target_compile_features(replay_acquisition_source_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_acquisition_source_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_acquisition_source_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_acquisition_source_tests)

add_executable(replay_controller_tests replay_controller_tests.cpp)
target_link_libraries(replay_controller_tests PRIVATE replay_controller replay_local_argb_stage local_argb doctest::doctest)
target_compile_features(replay_controller_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_controller_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_controller_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_controller_tests)

add_executable(replay_output_stage_tests replay_output_stage_tests.cpp)
target_link_libraries(replay_output_stage_tests PRIVATE replay_scheduler doctest::doctest)
target_compile_features(replay_output_stage_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_output_stage_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_output_stage_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_output_stage_tests)

add_executable(replay_scheduler_tests replay_scheduler_tests.cpp)
target_link_libraries(replay_scheduler_tests PRIVATE replay_scheduler replay_local_argb_stage local_argb doctest::doctest)
target_compile_features(replay_scheduler_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_scheduler_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_scheduler_tests)

add_executable(replay_playback_tests replay_playback_tests.cpp)
target_link_libraries(replay_playback_tests PRIVATE replay_playback replay_pixel_output replay_local_argb_stage local_argb doctest::doctest)
target_compile_features(replay_playback_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_playback_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_playback_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_playback_tests)

add_executable(replay_output_tests replay_output_tests.cpp)
target_link_libraries(replay_output_tests PRIVATE replay_pixel_output replay_local_argb_stage replay_scheduler local_argb mazda doctest::doctest)
target_compile_features(replay_output_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_output_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_output_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_output_tests)

add_executable(replay_signal_observer_tests replay_signal_observer_tests.cpp)
target_include_directories(replay_signal_observer_tests PRIVATE ${CMAKE_SOURCE_DIR}/lib/replay/src)
target_link_libraries(replay_signal_observer_tests PRIVATE replay_signal_output replay_pixel_output replay_local_argb_stage replay_scheduler mazda mazda_telemetry vehicle_signals doctest::doctest)
target_compile_features(replay_signal_observer_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(replay_signal_observer_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(replay_signal_observer_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(replay_signal_observer_tests)

add_executable(gvret_inspect_tests gvret_inspect_tests.cpp)
target_link_libraries(gvret_inspect_tests PRIVATE gvret_parser doctest::doctest)
target_compile_features(gvret_inspect_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(gvret_inspect_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_inspect_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(gvret_inspect_tests)

add_executable(can_replay_cli_tests can_replay_cli_tests.cpp)
target_link_libraries(can_replay_cli_tests PRIVATE doctest::doctest)
target_compile_features(can_replay_cli_tests PRIVATE cxx_std_17)
add_dependencies(can_replay_cli_tests can-replay)
if(MSVC)
  target_compile_options(can_replay_cli_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(can_replay_cli_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(can_replay_cli_tests)

add_executable(web_emulator_tests web_emulator_tests.cpp)
target_link_libraries(web_emulator_tests PRIVATE replay_web_emulator doctest::doctest)
target_compile_features(web_emulator_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(web_emulator_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(web_emulator_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(web_emulator_tests)

add_executable(web_emulator_assets_tests web_emulator_assets_tests.cpp)
target_link_libraries(web_emulator_assets_tests PRIVATE replay_web_emulator doctest::doctest)
target_compile_features(web_emulator_assets_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(web_emulator_assets_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(web_emulator_assets_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(web_emulator_assets_tests)

# The browser renderer core is plain JavaScript with no packages. Its unit
# tests run only where Node.js is already installed; CI does not require it.
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(
    NAME web_emulator_renderer_js_tests
    COMMAND
      ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web_emulator_renderer_tests.js
      ${PROJECT_SOURCE_DIR}/lib/replay/web/app.js
  )
endif()

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

# The generic action engine is tested against the host provider fake and a
# make-independent fixture catalog; no Mazda target is reachable here.
add_executable(action_engine_tests
  action_engine_condition_tests.cpp
  action_engine_range_tests.cpp
  action_engine_sampled_state_tests.cpp
  action_engine_rule_tests.cpp
  action_engine_tests.cpp
)
target_link_libraries(action_engine_tests PRIVATE action_engine vehicle_signals_test_support
                                                  action_engine_test_support doctest::doctest)
target_compile_features(action_engine_tests PRIVATE cxx_std_17)

# Application-composition proof: the only test that knows both the engine and
# the Mazda provider. It reuses the private host-service seam, like
# signal_consumer_tests, to drive real frames through the production path.
add_executable(action_engine_composition_tests action_engine_composition_tests.cpp)
target_include_directories(action_engine_composition_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(action_engine_composition_tests PRIVATE action_engine
                                                              action_engine_test_support
                                                              mazda_telemetry doctest::doctest)
target_compile_features(action_engine_composition_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(action_engine_tests PRIVATE /W4 /permissive-)
  target_compile_options(action_engine_composition_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(action_engine_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(action_engine_composition_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(action_engine_tests)
doctest_discover_tests(action_engine_composition_tests)

# Portable companion BLE protocol codec (#162). The codec, transfer and live
# signal tests use only the codec, the provider fake and fixture catalogs; the
# provider test alone reuses the private host-service seam, like
# signal_consumer_tests, to sample the production Mazda provider policy.
add_executable(companion_protocol_codec_tests companion_protocol_codec_tests.cpp)
target_link_libraries(companion_protocol_codec_tests PRIVATE companion_protocol doctest::doctest)
add_executable(companion_protocol_transfer_tests companion_protocol_transfer_tests.cpp)
target_link_libraries(companion_protocol_transfer_tests PRIVATE companion_protocol
                                                                doctest::doctest)
add_executable(companion_protocol_live_signals_tests companion_protocol_live_signals_tests.cpp)
target_link_libraries(companion_protocol_live_signals_tests PRIVATE companion_protocol
                                                                    vehicle_signals_test_support
                                                                    doctest::doctest)
add_executable(companion_protocol_provider_tests companion_protocol_provider_tests.cpp)
target_include_directories(companion_protocol_provider_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(companion_protocol_provider_tests PRIVATE companion_protocol mazda_telemetry
                                                                doctest::doctest)
# Pairing window, user-key debounce and link security policy (#164).
add_executable(companion_protocol_pairing_tests companion_protocol_pairing_tests.cpp)
target_link_libraries(companion_protocol_pairing_tests PRIVATE companion_protocol doctest::doctest)
foreach(companion_test IN ITEMS companion_protocol_codec_tests companion_protocol_transfer_tests
                                companion_protocol_live_signals_tests
                                companion_protocol_provider_tests
                                companion_protocol_pairing_tests)
  target_compile_features(${companion_test} PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(${companion_test} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${companion_test} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  doctest_discover_tests(${companion_test})
endforeach()

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

# Local LED action sink: engine commands to the renderer's private lighting
# handoff. No Mazda target is reachable from this test.
add_executable(local_led_action_sink_tests local_led_action_sink_tests.cpp)
target_link_libraries(local_led_action_sink_tests PRIVATE local_argb_actions doctest::doctest)
target_compile_features(local_led_action_sink_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(local_led_action_sink_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(local_led_action_sink_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(local_led_action_sink_tests)

# Host composition proof: generic provider fake -> engine -> LED sink ->
# production renderer state machine -> fake PixelFrameSink. The renderer is
# reached through the component's private include root; no Mazda target is
# reachable from this test.
add_executable(local_led_action_composition_tests local_led_action_composition_tests.cpp)
target_include_directories(local_led_action_composition_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/local_argb/private_include
)
target_link_libraries(local_led_action_composition_tests PRIVATE local_argb_actions local_argb
                                                                 vehicle_signals_test_support
                                                                 doctest::doctest)
target_compile_features(local_led_action_composition_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(local_led_action_composition_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(local_led_action_composition_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(local_led_action_composition_tests)

add_executable(rpm_level_fill_tests rpm_level_fill_tests.cpp)
target_link_libraries(rpm_level_fill_tests PRIVATE controller_config vehicle_signals_test_support
                                                   doctest::doctest)
target_compile_features(rpm_level_fill_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(rpm_level_fill_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(rpm_level_fill_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(rpm_level_fill_tests)

add_executable(rpm_threshold_tests rpm_threshold_tests.cpp)
target_link_libraries(rpm_threshold_tests PRIVATE controller_config vehicle_signals_test_support
                                                  doctest::doctest)
target_compile_features(rpm_threshold_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(rpm_threshold_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(rpm_threshold_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(rpm_threshold_tests)

add_executable(lighting_profile_tests lighting_profile_tests.cpp)
target_link_libraries(lighting_profile_tests PRIVATE controller_config doctest::doctest)
target_compile_features(lighting_profile_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(lighting_profile_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(lighting_profile_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(lighting_profile_tests)

add_executable(controller_config_schema_tests controller_config_schema_tests.cpp)
target_link_libraries(controller_config_schema_tests PRIVATE controller_config doctest::doctest)
target_compile_features(controller_config_schema_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(controller_config_schema_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(controller_config_schema_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(controller_config_schema_tests)

add_executable(lighting_profile_application_tests lighting_profile_application_tests.cpp)
target_link_libraries(lighting_profile_application_tests PRIVATE controller_config
                                                                   vehicle_signals_test_support
                                                                   doctest::doctest)
target_compile_features(lighting_profile_application_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(lighting_profile_application_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(lighting_profile_application_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(lighting_profile_application_tests)

add_executable(controller_config_json_tests controller_config_json_tests.cpp)
target_link_libraries(controller_config_json_tests PRIVATE controller_config doctest::doctest)
target_compile_features(controller_config_json_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_json_tests)

add_executable(controller_config_store_tests controller_config_store_tests.cpp)
target_include_directories(controller_config_store_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/controller_config/private_include
)
target_link_libraries(controller_config_store_tests PRIVATE controller_config doctest::doctest)
target_compile_features(controller_config_store_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_store_tests)

add_executable(controller_config_application_tests controller_config_application_tests.cpp)
set(_factory_yaml "${CMAKE_SOURCE_DIR}/config/default.yaml")
set(_factory_compiler "${CMAKE_SOURCE_DIR}/tools/compile_controller_config.py")
set(_factory_catalog "${CMAKE_SOURCE_DIR}/tools/controller_signal_catalog.json")
set(_factory_duration_contract
  "${CMAKE_SOURCE_DIR}/components/local_argb_sink_contract/include/local_argb/lighting_sink.hpp")
set(_factory_json "${CMAKE_CURRENT_BINARY_DIR}/factory_default_config.json")
# Exercise the same YAML compiler and canonical loader as the firmware. A
# source/compiler/catalog/contract edit must regenerate this document before testing.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_factory_yaml}" "${_factory_compiler}" "${_factory_catalog}" "${_factory_duration_contract}")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${_factory_compiler}" "${_factory_yaml}" "${_factory_json}"
  RESULT_VARIABLE _factory_compile_result
  OUTPUT_VARIABLE _factory_compile_output
  ERROR_VARIABLE _factory_compile_error)
if(NOT _factory_compile_result EQUAL 0)
  message(FATAL_ERROR "Host factory configuration compilation failed:\n"
    "${_factory_compile_output}${_factory_compile_error}")
endif()
target_include_directories(controller_config_application_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/board/include)
target_compile_definitions(controller_config_application_tests PRIVATE
  CONTROLLER_CONFIG_FACTORY_JSON_PATH="${_factory_json}")
target_link_libraries(controller_config_application_tests PRIVATE controller_config
                                                                  vehicle_signals_test_support
                                                                  doctest::doctest)
target_compile_features(controller_config_application_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_application_tests)
add_test(NAME controller_config_factory_build
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tools/controller_config_factory_build_test.py
          --cmake ${CMAKE_COMMAND} --compiler ${CMAKE_CXX_COMPILER})

# Companion config service (#165): wire-code tables, commit, revert, boot
# selection and the scratch dry run against the factory config and the real
# Mazda catalog.
add_executable(companion_config_tests companion_config_tests.cpp)
target_include_directories(companion_config_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_compile_definitions(companion_config_tests PRIVATE
  CONTROLLER_CONFIG_FACTORY_JSON_PATH="${_factory_json}")
target_link_libraries(companion_config_tests PRIVATE companion_config mazda
                                                     vehicle_signals_test_support
                                                     telemetry_test_support doctest::doctest)
target_compile_features(companion_config_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(companion_config_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(companion_config_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(companion_config_tests)

# Every successful compiler fixture is accepted by the production JSON loader
# and the action engine's resolvers against the real Mazda catalog.
add_executable(controller_config_json_check controller_config_json_check.cpp)
target_include_directories(controller_config_json_check PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(controller_config_json_check PRIVATE controller_config mazda)
target_compile_features(controller_config_json_check PRIVATE cxx_std_17)
add_test(NAME controller_config_yaml_loader
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/controller_config_yaml_tests.py
          --loader $<TARGET_FILE:controller_config_json_check>)

add_executable(controller_config_transient_tests controller_config_transient_tests.cpp)
target_include_directories(controller_config_transient_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/local_argb/private_include)
target_link_libraries(controller_config_transient_tests PRIVATE controller_config local_argb
  vehicle_signals_test_support doctest::doctest)
target_compile_features(controller_config_transient_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_transient_tests)

add_executable(controller_config_solid_tests controller_config_solid_tests.cpp)
target_include_directories(controller_config_solid_tests PRIVATE ${CMAKE_SOURCE_DIR}/components/local_argb/private_include)
target_link_libraries(controller_config_solid_tests PRIVATE controller_config local_argb vehicle_signals_test_support doctest::doctest)
target_compile_features(controller_config_solid_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_solid_tests)

# The reviewed selector-position example (#147), loaded from its canonical
# JSON and applied against the production Mazda signal catalog.
add_executable(controller_config_selector_example_tests
  controller_config_selector_example_tests.cpp)
target_include_directories(controller_config_selector_example_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_compile_definitions(controller_config_selector_example_tests PRIVATE
  CONTROLLER_CONFIG_SELECTOR_EXAMPLE_JSON_PATH="${CMAKE_SOURCE_DIR}/docs/specs/configuration/examples/selector-actions-v1.json")
target_link_libraries(controller_config_selector_example_tests PRIVATE controller_config mazda
                                                                       vehicle_signals_test_support
                                                                       doctest::doctest)
target_compile_features(controller_config_selector_example_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_selector_example_tests)

# Reviewed actual-gear example (not factory-enabled) applied through the
# persisted configuration path against the real Mazda catalog. YAML/JSON
# parity is checked by tests/tools/compile_controller_config_test.py.
add_executable(controller_config_actual_gear_tests controller_config_actual_gear_tests.cpp)
target_include_directories(controller_config_actual_gear_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
  ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_compile_definitions(controller_config_actual_gear_tests PRIVATE
  CONTROLLER_CONFIG_ACTUAL_GEAR_JSON_PATH="${CMAKE_SOURCE_DIR}/docs/specs/configuration/examples/actual-gear-actions-v1.json")
target_link_libraries(controller_config_actual_gear_tests PRIVATE controller_config mazda
  vehicle_signals_test_support action_engine_test_support doctest::doctest)
target_compile_features(controller_config_actual_gear_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(controller_config_actual_gear_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(controller_config_actual_gear_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(controller_config_actual_gear_tests)

# Reviewed door/liftgate open-action example (#149), outside the factory
# profile. The compiler tests keep its YAML and canonical JSON in parity.
add_executable(controller_config_door_actions_tests controller_config_door_actions_tests.cpp)
target_compile_definitions(controller_config_door_actions_tests PRIVATE
  CONTROLLER_CONFIG_DOOR_EXAMPLE_JSON_PATH="${CMAKE_SOURCE_DIR}/docs/specs/configuration/examples/door-liftgate-open-actions-v1.json")
target_link_libraries(controller_config_door_actions_tests PRIVATE controller_config
  vehicle_signals_test_support action_engine_test_support doctest::doctest)
target_compile_features(controller_config_door_actions_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_door_actions_tests)

# The reviewed lock-state example is not part of the factory profile; load its
# committed canonical JSON through the persisted application path and the real
# Mazda catalog.
set(_lock_state_example_json
  "${CMAKE_SOURCE_DIR}/docs/specs/configuration/examples/lock-state-actions-v1.json")
add_executable(controller_config_lock_state_tests controller_config_lock_state_tests.cpp)
target_compile_definitions(controller_config_lock_state_tests PRIVATE
  CONTROLLER_CONFIG_LOCK_STATE_JSON_PATH="${_lock_state_example_json}")
target_link_libraries(controller_config_lock_state_tests PRIVATE controller_config
                                                                 vehicle_signals_test_support
                                                                 doctest::doctest)
target_compile_features(controller_config_lock_state_tests PRIVATE cxx_std_17)
doctest_discover_tests(controller_config_lock_state_tests)
add_test(NAME controller_config_lock_state_example_loads
  COMMAND controller_config_json_check ${_lock_state_example_json})

# Reviewed indicator-lamp example (#151): not part of the factory profile. The
# checked-in canonical JSON is held in parity with its YAML source by the
# compiler regression tests and applied here through the persisted path.
add_executable(indicator_lamp_actions_tests indicator_lamp_actions_tests.cpp)
target_compile_definitions(indicator_lamp_actions_tests PRIVATE
  INDICATOR_LAMP_EXAMPLE_JSON_PATH="${CMAKE_SOURCE_DIR}/docs/specs/configuration/examples/indicator-lamp-actions.json")
target_link_libraries(indicator_lamp_actions_tests PRIVATE controller_config
                                                           vehicle_signals_test_support
                                                           doctest::doctest)
target_compile_features(indicator_lamp_actions_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(indicator_lamp_actions_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(indicator_lamp_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(indicator_lamp_actions_tests)

# Reviewed speed level example (#152): canonical JSON applied through the
# persisted configuration path; YAML parity is checked by the compiler tests.
add_executable(speed_level_example_tests speed_level_example_tests.cpp)
target_compile_definitions(speed_level_example_tests PRIVATE
  CONTROLLER_CONFIG_SPEED_LEVEL_EXAMPLE_JSON_PATH="${CMAKE_SOURCE_DIR}/docs/specs/configuration/examples/speed-level-example.json")
target_link_libraries(speed_level_example_tests PRIVATE controller_config
  vehicle_signals_test_support doctest::doctest)
target_compile_features(speed_level_example_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(speed_level_example_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(speed_level_example_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(speed_level_example_tests)

add_executable(companion_ble_uuid_tests companion_ble_uuid_tests.cpp)
target_link_libraries(companion_ble_uuid_tests PRIVATE companion_ble_uuids doctest::doctest)
target_compile_features(companion_ble_uuid_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(companion_ble_uuid_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(companion_ble_uuid_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
doctest_discover_tests(companion_ble_uuid_tests)
