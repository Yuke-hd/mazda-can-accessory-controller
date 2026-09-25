idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "."
  REQUIRES board local_argb local_argb_sink_contract local_argb_actions action_engine
           controller_config vehicle_can_rx mazda_telemetry freertos
)
