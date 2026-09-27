if(COMMAND idf_component_register)
  message(FATAL_ERROR "gvret_parser is host-only and cannot be built as an ESP-IDF component")
endif()

add_library(gvret_parser STATIC src/csv_parser.cpp src/replay.cpp src/replay_clock.cpp src/file_loader.cpp)
target_include_directories(gvret_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gvret_parser PUBLIC vehicle_core)
target_compile_features(gvret_parser PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(gvret_parser PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_parser PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(gvret_replay_source STATIC src/replay_acquisition_source.cpp)
target_include_directories(gvret_replay_source PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gvret_replay_source PUBLIC gvret_parser vehicle_telemetry)
target_compile_features(gvret_replay_source PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(gvret_replay_source PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_replay_source PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(gvret_replay_controller STATIC src/replay_controller.cpp)
target_include_directories(
  gvret_replay_controller
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_SOURCE_DIR}/components/local_argb/private_include
    ${CMAKE_SOURCE_DIR}/components/mazda_telemetry/private_include
    ${CMAKE_SOURCE_DIR}/lib/mazda/internal_include
)
target_link_libraries(
  gvret_replay_controller
  PUBLIC gvret_replay_source local_argb
  PRIVATE
    action_engine
    controller_config
    local_argb_actions
    local_argb_sink_contract
    mazda_telemetry
)
target_compile_features(gvret_replay_controller PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(gvret_replay_controller PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_replay_controller PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(gvret_replay_scheduler STATIC src/replay_scheduler.cpp)
target_include_directories(gvret_replay_scheduler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gvret_replay_scheduler PUBLIC gvret_replay_controller mazda controller_config)
target_compile_features(gvret_replay_scheduler PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(gvret_replay_scheduler PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_replay_scheduler PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(gvret_replay_output STATIC src/pixel_frame_output.cpp)
target_include_directories(gvret_replay_output PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gvret_replay_output PUBLIC gvret_parser local_argb_pixel_frame)
target_compile_features(gvret_replay_output PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(gvret_replay_output PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret_replay_output PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gvret-replay src/gvret_replay.cpp)
target_link_libraries(gvret-replay PRIVATE gvret_replay_output gvret_replay_scheduler)
target_compile_features(gvret-replay PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(gvret-replay PRIVATE /W4 /permissive-)
else()
  target_compile_options(gvret-replay PRIVATE -Wall -Wextra -Wpedantic)
endif()
