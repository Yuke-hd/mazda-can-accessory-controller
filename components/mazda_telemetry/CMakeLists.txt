if(COMMAND idf_component_register)
  idf_component_register(SRCS "src/vehicle_telemetry.cpp"
                         "src/public_contract_consumer.cpp"
                         "src/local_argb_binding.cpp"
                         INCLUDE_DIRS "include"
                         PRIV_INCLUDE_DIRS "private_include"
                                         "${CMAKE_CURRENT_LIST_DIR}/../../lib/mazda/internal_include"
                         REQUIRES vehicle_core mazda vehicle_telemetry
                         PRIV_REQUIRES local_argb local_argb_sink_contract)
  return()
endif()

# The publication/polling implementation is deliberately a module-local
# target. S2-A can consume its fixed publish/reset handoff without exporting
# private state, task handles, synchronization, or clock seams.
add_library(mazda_telemetry STATIC
  src/vehicle_telemetry.cpp
  src/public_contract_consumer.cpp
  src/local_argb_binding.cpp
)
find_package(Threads REQUIRED)
target_include_directories(
  mazda_telemetry
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
)
# The facade's value-only contracts are supplied by mazda; the store's
# implementation-only state/decoder includes remain private above.
target_link_libraries(mazda_telemetry PUBLIC mazda Threads::Threads vehicle_telemetry)
target_link_libraries(mazda_telemetry PRIVATE local_argb local_argb_sink_contract)
target_compile_features(mazda_telemetry PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(mazda_telemetry PRIVATE /W4 /permissive-)
else()
  target_compile_options(mazda_telemetry PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Keep the Stage 1.5 target name as a source/build compatibility seam. It
# carries the value-only public include root and the runtime publication
# implementation without propagating this component's private include path.
add_library(mazda_telemetry_contracts INTERFACE)
target_include_directories(
  mazda_telemetry_contracts
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(mazda_telemetry_contracts INTERFACE mazda_telemetry)

if(BUILD_TESTING AND MAZDA_BUILD_HOST_TESTS)
  add_executable(mazda_telemetry_publication_tests
    tests/publication_store_tests.cpp
  )
  target_include_directories(mazda_telemetry_publication_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
  )
  target_link_libraries(mazda_telemetry_publication_tests PRIVATE
    mazda_telemetry
    Threads::Threads
  )
  target_compile_features(mazda_telemetry_publication_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_publication_tests
           COMMAND mazda_telemetry_publication_tests)

  add_executable(mazda_telemetry_service_tests
    tests/vehicle_telemetry_service_tests.cpp
  )
  target_include_directories(mazda_telemetry_service_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
  )
  target_link_libraries(mazda_telemetry_service_tests PRIVATE
    mazda_telemetry
    Threads::Threads
  )
  target_compile_features(mazda_telemetry_service_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_service_tests
           COMMAND mazda_telemetry_service_tests)

  add_executable(mazda_telemetry_descriptor_tests
    tests/vehicle_telemetry_descriptor_tests.cpp
  )
  target_include_directories(mazda_telemetry_descriptor_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
  )
  target_link_libraries(mazda_telemetry_descriptor_tests PRIVATE
    mazda_telemetry
    Threads::Threads
  )
  target_compile_features(mazda_telemetry_descriptor_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_descriptor_tests
           COMMAND mazda_telemetry_descriptor_tests)

endif()
