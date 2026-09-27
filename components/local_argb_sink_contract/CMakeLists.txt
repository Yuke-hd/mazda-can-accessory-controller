if(COMMAND idf_component_register)
  idf_component_register(
    INCLUDE_DIRS "include" "frame_include"
    REQUIRES vehicle_core
  )
  return()
endif()

# Renderer-independent PixelFrame contract. It has its own include root so the
# ordinary local_argb target can re-export it without exporting the sink root.
add_library(local_argb_pixel_frame INTERFACE)
target_include_directories(
  local_argb_pixel_frame
  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/frame_include
)
target_compile_features(local_argb_pixel_frame INTERFACE cxx_std_17)

# Explicit implementation-only access for S2-A. This target exports the
# value-only sink directory and the frame contract; renderer.hpp remains
# component-private.
add_library(local_argb_sink_contract INTERFACE)
target_include_directories(
  local_argb_sink_contract
  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(local_argb_sink_contract INTERFACE vehicle_core local_argb_pixel_frame)
