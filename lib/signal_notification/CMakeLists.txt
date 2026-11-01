# Repository-owned latest-state notification delivery for signal providers.
# The public include root exposes only the value-copy contract
# (signal_notification/notification.hpp). The channel implementation lives in
# private_include and is granted by path, as a PRIVATE include, only to the
# Mazda and generated-profile providers and their tests.
if(COMMAND idf_component_register)
  idf_component_register(INCLUDE_DIRS "include" REQUIRES vehicle_core)
  return()
endif()

add_library(signal_notification INTERFACE)
target_include_directories(signal_notification INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(signal_notification INTERFACE vehicle_core)
target_compile_features(signal_notification INTERFACE cxx_std_17)

if(BUILD_TESTING AND MAZDA_BUILD_HOST_TESTS)
  find_package(Threads REQUIRED)
  add_executable(signal_notification_channel_tests tests/notification_channel_tests.cpp)
  target_include_directories(signal_notification_channel_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include)
  target_link_libraries(signal_notification_channel_tests PRIVATE
    signal_notification Threads::Threads)
  target_compile_features(signal_notification_channel_tests PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(signal_notification_channel_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(signal_notification_channel_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME signal_notification_channel_tests COMMAND signal_notification_channel_tests)
endif()
