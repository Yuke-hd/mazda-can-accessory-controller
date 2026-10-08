if(COMMAND idf_component_register)
  set(mazda_telemetry_idf_sources
      "src/clock.cpp"
      "src/publication_store.cpp"
      "src/vehicle_telemetry.cpp"
      "src/public_contract_consumer.cpp"
      "src/local_argb_binding.cpp"
      "src/signal_provider.cpp"
      "src/signal_provider_read.cpp"
      "src/signal_provider_subscriptions.cpp")
  if(CONFIG_WEACT_CAN_FRESHNESS_DEBUG)
    list(APPEND mazda_telemetry_idf_sources "src/debug_telemetry.cpp")
  endif()
  if(CONFIG_WEACT_CAN_TELEMETRY_BENCHMARK)
    list(APPEND mazda_telemetry_idf_sources "src/telemetry_synthetic_benchmark.cpp")
  endif()
  idf_component_register(SRCS ${mazda_telemetry_idf_sources}
                         INCLUDE_DIRS "include"
                         PRIV_INCLUDE_DIRS "private_include"
                                         "${CMAKE_CURRENT_LIST_DIR}/../../lib/mazda/internal_include"
                         REQUIRES vehicle_core mazda vehicle_telemetry vehicle_signals
                         PRIV_REQUIRES local_argb local_argb_sink_contract esp_timer)
  return()
endif()

# The publication/polling implementation is deliberately a module-local
# target. S2-A can consume its fixed publish/reset handoff without exporting
# private state, task handles, synchronization, or clock seams.
add_library(mazda_telemetry STATIC
  src/clock.cpp
  src/publication_store.cpp
  src/vehicle_telemetry.cpp
  src/debug_telemetry.cpp
  src/public_contract_consumer.cpp
  src/local_argb_binding.cpp
  src/signal_provider.cpp
  src/signal_provider_read.cpp
  src/signal_provider_subscriptions.cpp
)
option(MAZDA_ENABLE_DEBUG_TELEMETRY
       "Compile the opt-in host CAN freshness recorder" OFF)
if(MAZDA_ENABLE_DEBUG_TELEMETRY)
  # VehicleTelemetryService is an opaque placement-constructed type, but host
  # composition tests include its private access seam. Propagate the feature
  # definition so every translation unit sees the same class layout.
  target_compile_definitions(mazda_telemetry PUBLIC MAZDA_ENABLE_DEBUG_TELEMETRY=1)
endif()
option(MAZDA_ENABLE_TELEMETRY_PROFILING
       "Compile the opt-in host telemetry baseline hooks" OFF)
if(MAZDA_ENABLE_TELEMETRY_PROFILING)
  target_compile_definitions(mazda_telemetry PRIVATE MAZDA_ENABLE_TELEMETRY_PROFILING=1)
endif()
option(MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING
       "Compile the firmware-style bounded telemetry profiler for host coverage" OFF)
if(MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING)
  # The service layout changes when the fixed accumulator is enabled. Publish
  # the definition to every host consumer that includes the private service
  # seam so tests and the library use the same layout.
  target_compile_definitions(mazda_telemetry PUBLIC CONFIG_WEACT_CAN_TELEMETRY_PROFILING=1)
endif()
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
# The public MazdaSignalProvider declaration is expressed in vehicle_signals
# value types only.
target_link_libraries(mazda_telemetry PUBLIC mazda Threads::Threads vehicle_telemetry
                                             vehicle_signals)
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

  # Compile the production clock translation unit through its ESP branch with
  # a test-only esp_timer header/function stub. This checks the selected
  # platform implementation itself without changing the host library's
  # injected-clock behavior or exposing a public clock seam.
  add_executable(mazda_telemetry_esp_clock_tests
    tests/esp_clock_tests.cpp
    src/clock.cpp
    src/publication_store.cpp
  )
  target_compile_definitions(mazda_telemetry_esp_clock_tests PRIVATE ESP_PLATFORM=1)
  target_include_directories(mazda_telemetry_esp_clock_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/esp_timer_stub
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
  )
  target_link_libraries(mazda_telemetry_esp_clock_tests PRIVATE mazda Threads::Threads)
  target_compile_features(mazda_telemetry_esp_clock_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_esp_clock_tests
           COMMAND mazda_telemetry_esp_clock_tests)

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

  add_executable(mazda_telemetry_profiling_tests
    tests/telemetry_profiling_tests.cpp
  )
  target_include_directories(mazda_telemetry_profiling_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
  )
  target_link_libraries(mazda_telemetry_profiling_tests PRIVATE
    mazda_telemetry
  )
  target_compile_features(mazda_telemetry_profiling_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_profiling_tests
           COMMAND mazda_telemetry_profiling_tests)

  if(MAZDA_ENABLE_TELEMETRY_PROFILING OR MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING)
    add_executable(mazda_telemetry_group_tests
      tests/vehicle_telemetry_group_tests.cpp
    )
    target_include_directories(mazda_telemetry_group_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/private_include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
    )
    target_link_libraries(mazda_telemetry_group_tests PRIVATE
      mazda_telemetry
      Threads::Threads
    )
    target_compile_features(mazda_telemetry_group_tests PRIVATE cxx_std_17)
    add_test(NAME mazda_telemetry_group_tests
             COMMAND mazda_telemetry_group_tests)
  endif()

  add_executable(mazda_telemetry_signal_catalog_tests
    tests/signal_catalog_tests.cpp
  )
  target_include_directories(mazda_telemetry_signal_catalog_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
  )
  target_link_libraries(mazda_telemetry_signal_catalog_tests PRIVATE
    mazda_telemetry
    Threads::Threads
  )
  target_compile_features(mazda_telemetry_signal_catalog_tests PRIVATE cxx_std_17)
  # An exhaustive switch in the test keeps enum choice coverage in lock-step
  # with the Mazda enums: a new enumerator fails the build.
  if(NOT MSVC)
    target_compile_options(mazda_telemetry_signal_catalog_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror=switch)
  endif()
  add_test(NAME mazda_telemetry_signal_catalog_tests
           COMMAND mazda_telemetry_signal_catalog_tests)

  add_executable(mazda_telemetry_signal_provider_read_tests
    tests/signal_provider_read_tests.cpp
  )
  target_include_directories(mazda_telemetry_signal_provider_read_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
  )
  target_link_libraries(mazda_telemetry_signal_provider_read_tests PRIVATE
    mazda_telemetry
    Threads::Threads
  )
  target_compile_features(mazda_telemetry_signal_provider_read_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_signal_provider_read_tests
           COMMAND mazda_telemetry_signal_provider_read_tests)

  add_executable(mazda_telemetry_signal_subscription_tests
    tests/signal_provider_subscription_tests.cpp
  )
  target_include_directories(mazda_telemetry_signal_subscription_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
  )
  target_link_libraries(mazda_telemetry_signal_subscription_tests PRIVATE
    mazda_telemetry
    Threads::Threads
  )
  target_compile_features(mazda_telemetry_signal_subscription_tests PRIVATE cxx_std_17)
  add_test(NAME mazda_telemetry_signal_subscription_tests
           COMMAND mazda_telemetry_signal_subscription_tests)

  if(MAZDA_ENABLE_DEBUG_TELEMETRY)
    add_executable(mazda_telemetry_debug_telemetry_tests
      tests/debug_telemetry_tests.cpp
    )
    target_include_directories(mazda_telemetry_debug_telemetry_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/private_include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/mazda/internal_include
    )
    target_link_libraries(mazda_telemetry_debug_telemetry_tests PRIVATE
      mazda_telemetry
      Threads::Threads
    )
    target_compile_features(mazda_telemetry_debug_telemetry_tests PRIVATE cxx_std_17)
    add_test(NAME mazda_telemetry_debug_telemetry_tests
             COMMAND mazda_telemetry_debug_telemetry_tests)
  endif()

endif()
