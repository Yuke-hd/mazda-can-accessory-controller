# Round display ESP-NOW link (pairing v1 responder and telemetry v1 sender).
# sdk/ and include/pairing/ are copied byte for byte from the esp32-1-75
# display project's reviewed esp-now 2.5.3 adaptation; the firmware
# CMakeLists applies sdk/overlay.json before compilation. With
# CONFIG_DISPLAY_ESPNOW off, no source is compiled and nothing is linked.
set(display_espnow_sources "")
set(display_espnow_private_requires "")
if(CONFIG_DISPLAY_ESPNOW)
  set(display_espnow_sources "src/display_espnow.cpp")
  set(display_espnow_private_requires esp_wifi esp_netif esp_event esp_timer nvs_flash freertos log)
endif()
idf_component_register(SRCS ${display_espnow_sources}
                       INCLUDE_DIRS "include"
                       REQUIRES esp-now companion_protocol vehicle_signals
                       PRIV_REQUIRES ${display_espnow_private_requires})
