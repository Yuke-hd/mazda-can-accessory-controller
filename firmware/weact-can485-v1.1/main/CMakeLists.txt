idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "."
  REQUIRES board local_argb vehicle_can_rx mazda_telemetry freertos
)
