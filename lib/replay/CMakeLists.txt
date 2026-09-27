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

add_library(replay_controller STATIC src/controller.cpp)
target_include_directories(
  replay_controller
  PRIVATE
    ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
    ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(
  replay_controller
  PUBLIC replay_source replay_output_stage
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

add_executable(can-replay src/can_replay_cli.cpp)
target_link_libraries(
  can-replay
  PRIVATE gvret_parser replay_local_argb_stage replay_pixel_output replay_scheduler
)
target_compile_features(can-replay PRIVATE cxx_std_17)
replay_warnings(can-replay)
