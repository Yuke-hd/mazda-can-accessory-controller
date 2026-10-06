# Portable companion BLE protocol codec: device info, Command decoding, the
# chunked config transfer state machine, Config status, config read-back,
# live-signal frames and their notification pacing, plus the pairing window,
# user key debounce and link security policy that the BLE binding enforces. It
# links only the portable vehicle_signals contracts; no BLE stack, Mazda, CAN,
# RTOS, LED, storage or config-model target is reachable.
# Committing a config is an injected port, so the codec never parses, applies
# or persists one.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/active_document.cpp"
                              "src/command.cpp"
                              "src/config_status.cpp"
                              "src/config_transfer.cpp"
                              "src/crc32.cpp"
                              "src/device_info.cpp"
                              "src/live_signal_layout.cpp"
                              "src/link_security.cpp"
                              "src/live_signal_stream.cpp"
                              "src/live_signals.cpp"
                              "src/pairing_window.cpp"
                              "src/read_back.cpp"
                              "src/user_key_debouncer.cpp"
                         INCLUDE_DIRS "include"
                         REQUIRES vehicle_signals)
  return()
endif()

add_library(companion_protocol STATIC
  src/active_document.cpp
  src/command.cpp
  src/config_status.cpp
  src/config_transfer.cpp
  src/crc32.cpp
  src/device_info.cpp
  src/live_signal_layout.cpp
  src/link_security.cpp
  src/live_signal_stream.cpp
  src/live_signals.cpp
  src/pairing_window.cpp
  src/read_back.cpp
  src/user_key_debouncer.cpp
)
target_include_directories(companion_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(companion_protocol PUBLIC vehicle_signals)
target_compile_features(companion_protocol PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(companion_protocol PRIVATE /W4 /permissive-)
else()
  target_compile_options(companion_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()
