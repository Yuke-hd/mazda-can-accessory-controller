#include "display_espnow/display_espnow.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

#include "companion_protocol/display_link.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace display_espnow {

namespace {

constexpr char kTag[] = "display_espnow";
constexpr char kNamespace[] = "display_link";
constexpr char kRecordKey[] = "record";
constexpr std::uint64_t kPublishPeriodUs = 50'000; // 20 Hz (telemetry v1)
// Core 0 keeps core 1 for vehicle I/O (ADR-0004), like the companion link.
constexpr BaseType_t kStartupCore = 0;
constexpr UBaseType_t kStartupPriority = 1;
constexpr std::uint32_t kStartupStackBytes = 4096;

std::atomic<bool> started_once{false};
std::atomic<bool> telemetry_started{false};
const vehicle_signals::SignalProvider *provider{nullptr};
Inputs inputs{};
std::optional<companion_protocol::SnapshotPublisher> publisher;
std::array<std::uint8_t, 6> peer{};
esp_timer_handle_t timer{nullptr};

void wipe(void *bytes, std::size_t size) noexcept {
  volatile auto *cursor = static_cast<volatile std::uint8_t *>(bytes);
  for (std::size_t index = 0; index < size; ++index) {
    cursor[index] = 0;
  }
}

std::uint32_t random_word() noexcept { return esp_random(); }

std::optional<companion_protocol::PairingRecord> load_record() noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return std::nullopt;
  }
  companion_protocol::PairingRecordBytes bytes{};
  std::size_t size = bytes.size();
  const esp_err_t err = nvs_get_blob(handle, kRecordKey, bytes.data(), &size);
  nvs_close(handle);
  auto record = err == ESP_OK ? companion_protocol::decode_pairing_record(bytes.data(), size)
                              : std::nullopt;
  wipe(bytes.data(), bytes.size());
  return record;
}

void publish(void * /*argument*/) {
  if (telemetry_started.load(std::memory_order_relaxed)) {
    publisher->mark_telemetry_started();
  }
  const companion_protocol::DisplaySnapshot snapshot = publisher->next();
  // A lost or refused send is recovered by the next snapshot (telemetry v1).
  (void)esp_now_send(peer.data(), snapshot.data(), snapshot.size());
}

bool checked(esp_err_t err, const char *step) noexcept {
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "%s failed: %s; display link stays off", step, esp_err_to_name(err));
  }
  return err == ESP_OK;
}

bool start_wifi(std::uint8_t channel) noexcept {
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
    return checked(loop, "event loop");
  }
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  // Coexistence with BLE requires modem sleep, so the default power save
  // mode stays; a sender does not need to listen between snapshots.
  return checked(esp_netif_init(), "network initialization") &&
         checked(esp_wifi_init(&config), "Wi-Fi initialization") &&
         checked(esp_wifi_set_storage(WIFI_STORAGE_RAM), "Wi-Fi RAM storage") &&
         checked(esp_wifi_set_mode(WIFI_MODE_STA), "Wi-Fi station mode") &&
         checked(esp_wifi_start(), "Wi-Fi start") &&
         checked(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), "Wi-Fi channel");
}

bool start_espnow(companion_protocol::PairingRecord &record) noexcept {
  if (!checked(esp_now_init(), "ESP-NOW initialization")) {
    return false;
  }
  // Each device generates its own PMK (pairing v1); only the LMK is shared.
  std::uint8_t pmk[ESP_NOW_KEY_LEN];
  esp_fill_random(pmk, sizeof pmk);
  const esp_err_t pmk_err = esp_now_set_pmk(pmk);
  wipe(pmk, sizeof pmk);
  esp_now_peer_info_t config{};
  std::memcpy(config.peer_addr, record.peer.data(), record.peer.size());
  std::memcpy(config.lmk, record.lmk.data(), record.lmk.size());
  config.channel = record.channel;
  config.ifidx = WIFI_IF_STA;
  config.encrypt = true;
  const esp_err_t peer_err = pmk_err == ESP_OK ? esp_now_add_peer(&config) : pmk_err;
  wipe(config.lmk, sizeof config.lmk);
  if (!checked(peer_err, "encrypted peer")) {
    (void)esp_now_deinit();
    return false;
  }
  peer = record.peer;
  return true;
}

void startup(void * /*argument*/) {
  auto record = inputs.nvs_available ? load_record() : std::nullopt;
  if (!record.has_value()) {
    ESP_LOGI(kTag, "no saved display pairing; display link stays off");
  } else if (start_wifi(record->channel) && start_espnow(*record)) {
    publisher.emplace(*provider, random_word);
    const esp_timer_create_args_t args{publish, nullptr, ESP_TIMER_TASK, "display_tx", true};
    if (checked(esp_timer_create(&args, &timer), "publish timer") &&
        checked(esp_timer_start_periodic(timer, kPublishPeriodUs), "publish timer start")) {
      ESP_LOGI(kTag, "streaming to the paired display on channel %u",
               static_cast<unsigned>(record->channel));
    }
  }
  if (record.has_value()) {
    wipe(record->lmk.data(), record->lmk.size());
  }
  vTaskDelete(nullptr);
}

} // namespace

bool start(const vehicle_signals::SignalProvider &signal_provider, const Inputs &start_inputs) noexcept {
  if (started_once.exchange(true)) {
    return false;
  }
  provider = &signal_provider;
  inputs = start_inputs;
  return xTaskCreatePinnedToCore(startup, "display_start", kStartupStackBytes, nullptr,
                                 kStartupPriority, nullptr, kStartupCore) == pdPASS;
}

void mark_telemetry_started() noexcept { telemetry_started.store(true, std::memory_order_relaxed); }

} // namespace display_espnow
