# Portable extraction and decode-table contracts for generated profiles.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/extract.cpp" "src/provider.cpp" "src/notifications.cpp"
                         INCLUDE_DIRS "include"
                         PRIV_INCLUDE_DIRS "../signal_notification/private_include"
                         REQUIRES vehicle_core vehicle_signals signal_notification)
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
  return()
endif()

add_library(vehicle_profile_runtime STATIC src/extract.cpp src/provider.cpp src/notifications.cpp)
target_include_directories(vehicle_profile_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The notification channel implementation is provider-private.
target_include_directories(vehicle_profile_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../signal_notification/private_include)
target_link_libraries(vehicle_profile_runtime PUBLIC vehicle_core vehicle_signals
                                                     signal_notification)
target_compile_features(vehicle_profile_runtime PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(vehicle_profile_runtime PRIVATE /W4 /permissive-)
else()
  target_compile_options(vehicle_profile_runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Deterministic host replay drives the same provider processing and delivery
# path without importing the threaded receive service or Mazda internals.
add_library(vehicle_profile_replay_adapter STATIC src/replay_adapter.cpp)
target_include_directories(vehicle_profile_replay_adapter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../signal_notification/private_include)
target_link_libraries(vehicle_profile_replay_adapter PUBLIC vehicle_profile_runtime)
target_compile_features(vehicle_profile_replay_adapter PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(vehicle_profile_replay_adapter PRIVATE /W4 /permissive-)
else()
  target_compile_options(vehicle_profile_replay_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()
