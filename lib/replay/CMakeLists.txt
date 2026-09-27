if(COMMAND idf_component_register)
  message(FATAL_ERROR "replay is host-only and cannot be built as an ESP-IDF component")
endif()

function(replay_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_library(replay_clock STATIC src/clock.cpp)
target_include_directories(replay_clock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(replay_clock PUBLIC vehicle_core)
target_compile_features(replay_clock PUBLIC cxx_std_17)
replay_warnings(replay_clock)

add_library(replay_source STATIC src/acquisition_source.cpp)
target_link_libraries(replay_source PUBLIC replay_clock gvret_parser vehicle_telemetry)
replay_warnings(replay_source)

# The output-stage interface the replay core drives. It names no output
# technology; concrete stages are separate targets.
add_library(replay_output_stage INTERFACE)
target_include_directories(replay_output_stage INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(replay_output_stage INTERFACE action_engine vehicle_core)
target_compile_features(replay_output_stage INTERFACE cxx_std_17)

# The signal-observer interface. It names only portable vehicle_signals types.
add_library(replay_signal_observer INTERFACE)
target_include_directories(replay_signal_observer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(replay_signal_observer INTERFACE vehicle_signals vehicle_core)
target_compile_features(replay_signal_observer INTERFACE cxx_std_17)

add_library(replay_controller STATIC src/controller.cpp src/signal_observer_fan_out.cpp)
target_include_directories(
  replay_controller
  PRIVATE
    ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
    ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(
  replay_controller
  PUBLIC replay_source replay_output_stage replay_signal_observer
  PRIVATE action_engine mazda_telemetry
)
replay_warnings(replay_controller)

add_library(replay_scheduler STATIC src/scheduler.cpp)
target_link_libraries(replay_scheduler PUBLIC replay_controller mazda controller_config)
replay_warnings(replay_scheduler)

# The local ARGB output stage: the only replay target that sees the local ARGB
# renderer internals.
add_library(replay_local_argb_stage STATIC src/local_argb_stage.cpp)
target_include_directories(
  replay_local_argb_stage
  PRIVATE ${CMAKE_SOURCE_DIR}/components/local_argb/private_include
)
target_link_libraries(
  replay_local_argb_stage
  PUBLIC replay_output_stage local_argb_pixel_frame
  PRIVATE controller_config local_argb local_argb_actions local_argb_sink_contract
)
replay_warnings(replay_local_argb_stage)

# PixelFrameSink outputs of the local ARGB stage. They depend on the frame
# contract only, not on the renderer.
add_library(replay_pixel_output STATIC src/pixel_frame_output.cpp)
target_link_libraries(replay_pixel_output PUBLIC replay_clock local_argb_pixel_frame)
replay_warnings(replay_pixel_output)

# JSONL "signal" records for SignalObservers.
add_library(replay_signal_output STATIC src/signal_record_output.cpp)
target_link_libraries(replay_signal_output PUBLIC replay_signal_observer)
replay_warnings(replay_signal_output)

# Local-only browser adapter. It reuses the production replay scheduler and
# JSONL pixel sink through a WebSocket line transport; no ESP-IDF or renderer
# internals cross this host-only boundary.
# Browser assets are ordinary files under web/ so they can be edited and unit
# tested directly; configure time embeds them as raw string literals.
set(REPLAY_WEB_DELIMITER "WEBASSET")
set(REPLAY_WEB_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
foreach(asset IN ITEMS index.html style.css app.js)
  set(asset_path ${REPLAY_WEB_ASSET_DIR}/${asset})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${asset_path})
  file(READ ${asset_path} asset_body)
  # MSVC rejects string literals longer than 16,380 bytes (C2026).
  string(LENGTH "${asset_body}" asset_length)
  if(asset_length GREATER 16000)
    message(FATAL_ERROR "${asset_path} is ${asset_length} bytes; split it below 16000 bytes")
  endif()
  string(FIND "${asset_body}" ")${REPLAY_WEB_DELIMITER}\"" delimiter_position)
  if(NOT delimiter_position EQUAL -1)
    message(FATAL_ERROR "${asset_path} contains the raw string delimiter ${REPLAY_WEB_DELIMITER}")
  endif()
  string(MAKE_C_IDENTIFIER "REPLAY_WEB_${asset}" asset_variable)
  string(TOUPPER ${asset_variable} asset_variable)
  set(${asset_variable} "${asset_body}")
endforeach()
configure_file(
  src/web_emulator_assets.cpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/web_emulator_assets.cpp
  @ONLY
)

add_library(
  replay_web_emulator STATIC
  src/web_emulator.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/web_emulator_assets.cpp
)
target_include_directories(replay_web_emulator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
  replay_web_emulator
  PUBLIC gvret_parser replay_scheduler replay_local_argb_stage replay_pixel_output
)
if(WIN32)
  target_link_libraries(replay_web_emulator PUBLIC ws2_32)
endif()
target_compile_features(replay_web_emulator PUBLIC cxx_std_17)
replay_warnings(replay_web_emulator)

add_executable(can-replay src/can_replay_cli.cpp)
target_link_libraries(
  can-replay
  PRIVATE
    gvret_parser
    replay_local_argb_stage
    replay_pixel_output
    replay_scheduler
    replay_signal_output
)
target_compile_features(can-replay PRIVATE cxx_std_17)
replay_warnings(can-replay)

add_executable(gvret-led-emulator src/gvret_led_emulator.cpp)
target_link_libraries(gvret-led-emulator PRIVATE replay_web_emulator gvret_parser)
target_compile_features(gvret-led-emulator PRIVATE cxx_std_17)
replay_warnings(gvret-led-emulator)
