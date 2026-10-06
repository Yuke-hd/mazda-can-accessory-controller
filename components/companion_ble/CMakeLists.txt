# Companion BLE service: the NimBLE binding for the portable companion
# protocol codec in lib/companion_protocol. It owns NimBLE initialization,
# advertising and GATT registration; the codec owns every wire value.
if(COMMAND idf_component_register)
  idf_component_register(
    SRCS "src/gatt_service.cpp" "src/device_info_characteristic.cpp" "src/advertising.cpp"
         "src/companion_ble.cpp" "src/security.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES companion_protocol
    PRIV_REQUIRES bt esp_timer freertos log
  )
  return()
endif()

# Host builds cover only the header-only companion UUIDs; the NimBLE binding
# is firmware-only.
add_library(companion_ble_uuids INTERFACE)
target_include_directories(companion_ble_uuids INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(companion_ble_uuids INTERFACE cxx_std_17)
