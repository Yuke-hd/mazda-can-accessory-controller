# Portable extraction and decode-table contracts for generated profiles.
if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/extract.cpp" "src/provider.cpp"
                         INCLUDE_DIRS "include"
                         REQUIRES vehicle_core vehicle_signals)
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
  return()
endif()

add_library(vehicle_profile_runtime STATIC src/extract.cpp src/provider.cpp)
target_include_directories(vehicle_profile_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vehicle_profile_runtime PUBLIC vehicle_core vehicle_signals)
target_compile_features(vehicle_profile_runtime PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(vehicle_profile_runtime PRIVATE /W4 /permissive-)
else()
  target_compile_options(vehicle_profile_runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()
