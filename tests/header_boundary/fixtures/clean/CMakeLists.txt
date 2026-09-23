cmake_minimum_required(VERSION 3.20)
project(header_boundary_fixture LANGUAGES CXX)

add_library(mazda_contracts INTERFACE)
target_include_directories(mazda_contracts INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/mazda/include
)

add_library(vehicle_telemetry_contracts INTERFACE)
target_include_directories(vehicle_telemetry_contracts INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/components/vehicle_telemetry/include
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/mazda/include
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/vehicle_core/include
)
target_link_libraries(vehicle_telemetry_contracts INTERFACE mazda_contracts)
