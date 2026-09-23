idf_component_register(
  SRCS "src/driver_binding.cpp"
  INCLUDE_DIRS "include"
  REQUIRES board can_bus
  PRIV_REQUIRES esp_driver_twai
  WHOLE_ARCHIVE
)
