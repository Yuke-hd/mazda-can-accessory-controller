# Companion config service: the controller side of the companion BLE config
# transfer. It implements the companion protocol's commit, boot-selection and
# revert ports over the controller configuration (parse, dry-run apply,
# save_override and clear_override), so the BLE binding never sees a
# configuration, engine or LED type. It reaches the action engine and the LED
# action sink only to build never-attached scratch copies for the dry run. It
# has no BLE, CAN, RTOS or LED-driver dependency. Every controller-to-wire code
# mapping is an exhaustive switch, and -Werror=switch makes a new controller
# enumerator a build failure until it is mapped.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/config_service.cpp" "src/scratch_apply_check.cpp"
                              "src/wire_codes.cpp"
                         INCLUDE_DIRS "include"
                         REQUIRES action_engine companion_protocol controller_config
                                  local_argb_actions local_argb_sink_contract vehicle_core
                                  vehicle_signals)
  target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=switch)
  return()
endif()

add_library(companion_config STATIC
  src/config_service.cpp
  src/scratch_apply_check.cpp
  src/wire_codes.cpp
)
target_include_directories(companion_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(companion_config PUBLIC action_engine companion_protocol controller_config
                                              local_argb_actions local_argb_sink_contract)
target_compile_features(companion_config PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(companion_config PRIVATE /W4 /permissive- /we4062)
else()
  target_compile_options(companion_config PRIVATE -Wall -Wextra -Wpedantic -Werror=switch)
endif()
