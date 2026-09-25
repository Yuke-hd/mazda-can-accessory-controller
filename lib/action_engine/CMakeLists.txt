# Generic action engine: resolves persisted rules against a provider catalog,
# evaluates state/event rules on latest-state notices and fans commands out to
# action sinks. It links only the portable vehicle_signals contracts; no make,
# CAN, driver, RTOS, LED or transport component is reachable from this target.
# Host-only for now; firmware composition registers it separately.
add_library(action_engine STATIC
  src/actionability.cpp
  src/condition.cpp
  src/engine.cpp
  src/range_rule.cpp
  src/range_rule_set.cpp
  src/rule_set.cpp
  src/rules.cpp
  src/sink_fan_out.cpp
  src/subscription_set.cpp
)
target_include_directories(action_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(action_engine PUBLIC vehicle_signals)
target_compile_features(action_engine PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(action_engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(action_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
