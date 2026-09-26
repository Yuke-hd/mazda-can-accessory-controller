# Controller configuration: the application's configured features, expressed
# as generic engine rules and LED action bindings. It owns the feature choices,
# such as which signal drives which effect and over which input range, so
# neither the action engine nor the LED components learn about a signal. Only
# the firmware composition root applies it.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/lighting_profile_application.cpp" "src/rpm_level_fill.cpp"
                              "src/rpm_threshold.cpp"
                         INCLUDE_DIRS "include"
                         REQUIRES action_engine local_argb_actions)
  return()
endif()

add_library(controller_config STATIC
  src/lighting_profile_application.cpp
  src/rpm_level_fill.cpp
  src/rpm_threshold.cpp
)
target_include_directories(controller_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(controller_config PUBLIC action_engine local_argb_actions)
target_compile_features(controller_config PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(controller_config PRIVATE /W4 /permissive-)
else()
  target_compile_options(controller_config PRIVATE -Wall -Wextra -Wpedantic)
endif()
