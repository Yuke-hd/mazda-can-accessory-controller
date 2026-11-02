# Keep selected settings in configure-global properties for the regular
# component pass. ESP-IDF's separate early-expansion script cannot read them.
foreach(_project_setting VEHICLE_PROFILE_IS_NATIVE VEHICLE_PROFILE_MAPPING_PATH
                         VEHICLE_PROFILE_ARTIFACT_DIR VEHICLE_PROFILE_COMPILER_CATALOG
                         VEHICLE_PROFILE_METADATA_HEADER VEHICLE_PROFILE_RUNTIME_HEADER
                         VEHICLE_PROFILE_INCLUDE_DIR VEHICLE_PROFILE_CAPABILITY_REPORT
                         VEHICLE_PROFILE_ARTIFACT_TARGET PROFILE_LINK_PROBE_ENABLED
                         PROFILE_LINK_PROBE_GENERATED_DIR OUTPUT_CATALOG_JSON
                         OUTPUT_CATALOG_INCLUDE_DIR)
  get_property(${_project_setting} GLOBAL PROPERTY ${_project_setting})
endforeach()

if(CONFIG_WEACT_CAN_TELEMETRY_BENCHMARK)
  set(main_sources "telemetry_synthetic_benchmark_main.cpp")
else()
  set(main_sources "main.cpp")
  # Keep diagnostic tasks and their state out of production images entirely.
  if(VEHICLE_PROFILE_IS_NATIVE AND CONFIG_WEACT_CAN_FRESHNESS_DEBUG)
    list(APPEND main_sources "freshness_debug_logger.cpp")
  endif()
  if(VEHICLE_PROFILE_IS_NATIVE AND CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
    list(APPEND main_sources "telemetry_profiling_logger.cpp")
  endif()
  if(NOT VEHICLE_PROFILE_IS_NATIVE
     AND (CONFIG_WEACT_CAN_FRESHNESS_DEBUG OR CONFIG_WEACT_CAN_TELEMETRY_PROFILING))
    message(STATUS
      "Generated profile selected: Mazda-only freshness and stage profiling loggers are omitted; "
      "FreeRTOS task statistics remain available without telemetry pause counters")
  endif()
endif()

if(PROFILE_LINK_PROBE_ENABLED)
  list(APPEND main_sources
       "${CMAKE_CURRENT_LIST_DIR}/../../../tests/support/vehicle_profile_link_probe.cpp")
endif()

if(CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS)
  list(APPEND main_sources "freertos_runtime_stats_logger.cpp")
endif()

set(main_requires board local_argb local_argb_sink_contract local_argb_actions action_engine
                  controller_config vehicle_can_rx freertos vehicle_core esp_timer companion_ble
                  companion_config esp_app_format)
if(CMAKE_BUILD_EARLY_EXPANSION)
  # ESP-IDF resolves component requirements in a separate cmake -P process
  # before selected-profile variables are available. Make both candidate
  # services discoverable in that pass; the normal configure links one.
  list(APPEND main_requires mazda_telemetry vehicle_profile_runtime vehicle_telemetry)
elseif(VEHICLE_PROFILE_IS_NATIVE)
  list(APPEND main_requires mazda_telemetry)
else()
  list(APPEND main_requires vehicle_profile_runtime vehicle_telemetry)
endif()
if(PROFILE_LINK_PROBE_ENABLED AND VEHICLE_PROFILE_IS_NATIVE)
  list(APPEND main_requires vehicle_profile_runtime)
endif()

set(main_linker_fragments "hot_rodata.lf")
if(VEHICLE_PROFILE_IS_NATIVE)
  list(APPEND main_linker_fragments "hot_code.lf")
endif()

idf_component_register(
  SRCS ${main_sources}
  INCLUDE_DIRS "."
  REQUIRES ${main_requires}
  PRIV_REQUIRES esp_driver_twai
  EMBED_FILES "${OUTPUT_CATALOG_JSON}"
  # Flash-cache layout; see docs/development/flash-cache-layout.md.
  LDFRAGMENTS ${main_linker_fragments}
)
add_dependencies(${COMPONENT_LIB} output_catalog)
if(VEHICLE_PROFILE_ARTIFACT_TARGET)
  add_dependencies(${COMPONENT_LIB} ${VEHICLE_PROFILE_ARTIFACT_TARGET})
endif()
if(NOT VEHICLE_PROFILE_IS_NATIVE)
  target_include_directories(${COMPONENT_LIB} PRIVATE "${VEHICLE_PROFILE_INCLUDE_DIR}")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE VEHICLE_PROFILE_IS_NATIVE=0)
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE VEHICLE_PROFILE_IS_NATIVE=1)
endif()
if(PROFILE_LINK_PROBE_ENABLED)
  target_include_directories(${COMPONENT_LIB} PRIVATE "${PROFILE_LINK_PROBE_GENERATED_DIR}")
endif()
