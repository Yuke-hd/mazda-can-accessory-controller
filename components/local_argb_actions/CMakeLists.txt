# Local LED action sink: adapts generic action_engine commands to the local
# renderer's private LightingCommand handoff. It links the engine's action port
# and the authorized sink contract only; no vehicle make, signal provider, CAN
# or RTOS target is reachable from it. Host-only for now; firmware composition
# registers it separately.
add_library(local_argb_actions STATIC
  src/effect_bindings.cpp
  src/led_action_sink.cpp
)
target_include_directories(local_argb_actions PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(local_argb_actions PUBLIC action_engine local_argb_sink_contract)
target_compile_features(local_argb_actions PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(local_argb_actions PRIVATE /W4 /permissive-)
else()
  target_compile_options(local_argb_actions PRIVATE -Wall -Wextra -Wpedantic)
endif()
