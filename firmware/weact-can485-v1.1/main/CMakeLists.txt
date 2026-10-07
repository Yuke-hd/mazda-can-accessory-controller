set(main_sources "main.cpp")
# Keep the diagnostic task and its state out of production images entirely.
if(CONFIG_WEACT_CAN_FRESHNESS_DEBUG)
  list(APPEND main_sources "freshness_debug_logger.cpp")
endif()

idf_component_register(
  SRCS ${main_sources}
  INCLUDE_DIRS "."
  REQUIRES board local_argb local_argb_sink_contract local_argb_actions action_engine
           controller_config vehicle_can_rx mazda_telemetry freertos vehicle_core esp_timer
           companion_ble companion_config esp_app_format
  PRIV_REQUIRES esp_driver_twai
)
