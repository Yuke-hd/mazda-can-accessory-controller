#include "companion_ble/companion_ble.hpp"

#include "companion_ble/advertising.hpp"
#include "companion_ble/config_characteristics.hpp"
#include "companion_ble/device_info_characteristic.hpp"
#include "companion_ble/gatt_service.hpp"
#include "companion_ble/security.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <atomic>

namespace companion_ble {
namespace {

constexpr const char *kTag = "companion_ble";
// The complete local name in the scan response (ble-protocol.md).
constexpr const char *kDeviceName = "Mazda CAN Controller";

// The startup task only runs NimBLE initialization, the one serialization of
// the active config (prepare_boot()) and GATT registration, then deletes
// itself; the NimBLE host task is created by the ESP-IDF port. The
// serialization needs more than the 4 KiB the task had before #165; the task
// logs its headroom, which is not yet measured on hardware.
constexpr std::uint32_t kStartupTaskStackBytes = 6144U;
constexpr UBaseType_t kStartupTaskPriority = 1U;

std::atomic<bool> started{false};
// The encoded Device info values are served from here for the lifetime of the
// host. They are written once, before the startup task exists.
internal::DeviceInfoValues served_device_info{};
// Copied once by start(), read by the startup task.
PairingInputs pairing_inputs{};
ConfigInputs config_inputs{};
bool config_transfer{false};

void host_task(void * /*argument*/) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void on_host_sync() {
  internal::on_host_synced();
  const int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGW(kTag, "no usable BLE identity address (rc=%d)", rc);
    return;
  }
  internal::start_advertising();
}

void on_host_reset(const int reason) { ESP_LOGW(kTag, "NimBLE host reset (reason=%d)", reason); }

int register_services() noexcept {
  int rc = config_transfer
               ? internal::register_companion_service(
                     {internal::device_info_characteristic(served_device_info),
                      internal::config_characteristic(), internal::config_status_characteristic(),
                      internal::command_characteristic()})
               : internal::register_companion_service(
                     {internal::device_info_characteristic(served_device_info)});
  if (rc != 0)
    return rc;
  rc = ble_svc_gap_device_name_set(kDeviceName);
  return rc;
}

bool bring_up_nimble() noexcept {
  const esp_err_t init_result = nimble_port_init();
  if (init_result != ESP_OK) {
    ESP_LOGW(kTag, "NimBLE init failed: %s", esp_err_to_name(init_result));
    return false;
  }
  config_transfer = internal::prepare_config_transfer(config_inputs);
  if (!config_transfer)
    ESP_LOGW(kTag, "config ports missing; serving Device info only");
  else if (!internal::configure_config_transfer()) {
    ESP_LOGW(kTag, "config transfer unavailable; serving Device info only");
    config_transfer = false;
  }
  const int rc = register_services();
  if (rc != 0) {
    ESP_LOGW(kTag, "companion GATT registration failed (rc=%d)", rc);
    if (config_transfer)
      internal::release_config_transfer();
    nimble_port_deinit();
    return false;
  }
  // After registration, so a failed registration deinitializes NimBLE before
  // any policy timer exists.
  internal::configure_security(pairing_inputs);
  ble_hs_cfg.sync_cb = on_host_sync;
  ble_hs_cfg.reset_cb = on_host_reset;
  nimble_port_freertos_init(host_task);
  return true;
}

void startup_task(void * /*argument*/) {
  if (bring_up_nimble())
    ESP_LOGI(kTag, "companion BLE service started");
  ESP_LOGI(kTag, "startup task stack headroom: %u bytes",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  vTaskDelete(nullptr);
}

} // namespace

bool start(const companion_protocol::DeviceInfo &device_info, const PairingInputs &pairing,
           const ConfigInputs &config) noexcept {
  if (started.exchange(true))
    return false;
  companion_protocol::DeviceInfo served = device_info;
  served.pairing_window_open = false;
  const auto closed = companion_protocol::encode_device_info(served);
  served.pairing_window_open = true;
  const auto open = companion_protocol::encode_device_info(served);
  if (!closed.has_value() || !open.has_value()) {
    ESP_LOGW(kTag, "Device info string longer than %u bytes",
             static_cast<unsigned>(companion_protocol::kMaxDeviceInfoTextBytes));
    return false;
  }
  served_device_info.window_closed = *closed;
  served_device_info.window_open = *open;
  pairing_inputs = pairing;
  config_inputs = config;
  const BaseType_t created = xTaskCreate(startup_task, "companion_ble", kStartupTaskStackBytes,
                                         nullptr, kStartupTaskPriority, nullptr);
  return created == pdPASS;
}

} // namespace companion_ble
