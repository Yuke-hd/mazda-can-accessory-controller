if(COMMAND idf_component_register)
  idf_component_register(
    SRCS "src/legacy_compat.cpp" "src/legacy_submit_idf.cpp"
    INCLUDE_DIRS "include"
    REQUIRES vehicle_core mazda local_argb local_argb_sink_contract
  )
  return()
endif()

# Firmware-only compatibility target. The ordinary local_argb target neither
# compiles this source nor exports this include directory.
add_library(local_argb_firmware_compat STATIC
  src/legacy_compat.cpp
)
target_include_directories(
  local_argb_firmware_compat
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(local_argb_firmware_compat PUBLIC local_argb mazda vehicle_core)
target_compile_features(local_argb_firmware_compat PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(local_argb_firmware_compat PRIVATE /W4 /permissive-)
else()
  target_compile_options(local_argb_firmware_compat PRIVATE -Wall -Wextra -Wpedantic)
endif()
