idf_component_register(
  SRCS "src/board.cpp"
  INCLUDE_DIRS "include"
  # board.cpp directly includes ESP-IDF's GPIO driver API.
  REQUIRES esp_driver_gpio
)
