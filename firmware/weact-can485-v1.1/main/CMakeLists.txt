if(CONFIG_WEACT_CAN_TELEMETRY_BENCHMARK)
  set(main_sources "telemetry_synthetic_benchmark_main.cpp")
else()
  set(main_sources "main.cpp")
  # Keep diagnostic tasks and their state out of production images entirely.
  if(CONFIG_WEACT_CAN_FRESHNESS_DEBUG)
    list(APPEND main_sources "freshness_debug_logger.cpp")
  endif()
  if(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
    list(APPEND main_sources "telemetry_profiling_logger.cpp")
  endif()
endif()

if(CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS)
  list(APPEND main_sources "freertos_runtime_stats_logger.cpp")
endif()

idf_component_register(
  SRCS ${main_sources}
  INCLUDE_DIRS "."
  REQUIRES board local_argb local_argb_sink_contract local_argb_actions action_engine
           controller_config vehicle_can_rx mazda_telemetry freertos vehicle_core esp_timer
           companion_ble companion_config esp_app_format
  PRIV_REQUIRES esp_driver_twai
)
