if(COMMAND idf_component_register)
  idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES vehicle_core
  )
  return()
endif()

# Explicit implementation-only access for S2-A. This target exports only the
# value-only sink directory; renderer.hpp remains component-private.
add_library(local_argb_sink_contract INTERFACE)
target_include_directories(
  local_argb_sink_contract
  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(local_argb_sink_contract INTERFACE vehicle_core)
