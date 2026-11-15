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
#include "espnow.h"
#include "espnow_security.h"
#include "espnow_security_handshake.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pairing/setup_config.hpp"

namespace display_espnow {

namespace {

constexpr char kTag[] = "display_espnow";
constexpr char kNamespace[] = "display_link";
constexpr char kRecordKey[] = "record";
constexpr char kSetupFlagKey[] = "setup";
constexpr char kSetupCodeKey[] = "setup_code";
constexpr std::uint64_t kPublishPeriodUs = 50'000;  // 20 Hz (telemetry v1)
constexpr std::uint64_t kUserKeyPeriodUs = 20'000;  // kLongPressSamples basis
constexpr std::uint32_t kPairingWindowMs = 120'000; // pairing v1
constexpr int kMaxPairingFailures = 3;              // pairing v1
constexpr std::uint8_t kUnpairedSetupChannel = 1;
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
esp_timer_handle_t publish_timer{nullptr};
esp_timer_handle_t user_key_timer{nullptr};
companion_protocol::LongPressDetector long_press{};

// Setup boot only.
TaskHandle_t setup_task{nullptr};
std::uint8_t setup_channel{0};
std::atomic<int> pairing_failures{0};
std::atomic<bool> setup_saved{false};
// protocomm borrows the PoP bytes for the responder's lifetime.
std::array<char, companion_protocol::kSetupCodeChars + 1> pop{};

void wipe(void *bytes, std::size_t size) noexcept {
  volatile auto *cursor = static_cast<volatile std::uint8_t *>(bytes);
  for (std::size_t index = 0; index < size; ++index) {
    cursor[index] = 0;
  }
}

std::uint32_t random_word() noexcept { return esp_random(); }

bool checked(esp_err_t err, const char *step) noexcept {
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "%s failed: %s; display link stays off", step, esp_err_to_name(err));
  }
  return err == ESP_OK;
}

// ---- NVS --------------------------------------------------------------------

std::optional<companion_protocol::PairingRecord> load_record() noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return std::nullopt;
  }
  companion_protocol::PairingRecordBytes bytes{};
  std::size_t size = bytes.size();
  const esp_err_t err = nvs_get_blob(handle, kRecordKey, bytes.data(), &size);
  nvs_close(handle);
  auto record =
      err == ESP_OK ? companion_protocol::decode_pairing_record(bytes.data(), size) : std::nullopt;
  wipe(bytes.data(), bytes.size());
  return record;
}

bool save_record(const companion_protocol::PairingRecord &record) noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  companion_protocol::PairingRecordBytes bytes = companion_protocol::encode_pairing_record(record);
  esp_err_t err = nvs_set_blob(handle, kRecordKey, bytes.data(), bytes.size());
  wipe(bytes.data(), bytes.size());
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

bool write_setup_flag(bool set) noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t err = set ? nvs_set_u8(handle, kSetupFlagKey, 1) : nvs_erase_key(handle, kSetupFlagKey);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

bool setup_flag_set() noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  std::uint8_t value = 0;
  const bool set = nvs_get_u8(handle, kSetupFlagKey, &value) == ESP_OK && value == 1;
  nvs_close(handle);
  return set;
}

// Reads the flash-time provisioned code into `pop` as a NUL-terminated PoP.
bool load_setup_code() noexcept {
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  std::array<char, 40> text{};
  std::size_t size = text.size();
  const esp_err_t err = nvs_get_str(handle, kSetupCodeKey, text.data(), &size);
  nvs_close(handle);
  const auto code = err == ESP_OK && size > 0
                        ? companion_protocol::normalize_setup_code({text.data(), size - 1})
                        : std::nullopt;
  wipe(text.data(), text.size());
  if (!code.has_value()) {
    return false;
  }
  std::memcpy(pop.data(), code->data(), code->size());
  pop[code->size()] = '\0';
  return true;
}

// ---- Radio ------------------------------------------------------------------

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

// ---- Streaming mode ---------------------------------------------------------

void publish(void * /*argument*/) {
  if (telemetry_started.load(std::memory_order_relaxed)) {
    publisher->mark_telemetry_started();
  }
  const companion_protocol::DisplaySnapshot snapshot = publisher->next();
  // A lost or refused send is recovered by the next snapshot (telemetry v1).
  (void)esp_now_send(peer.data(), snapshot.data(), snapshot.size());
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

void stream(companion_protocol::PairingRecord &record) noexcept {
  if (!start_wifi(record.channel) || !start_espnow(record)) {
    return;
  }
  publisher.emplace(*provider, random_word);
  const esp_timer_create_args_t args{publish, nullptr, ESP_TIMER_TASK, "display_tx", true};
  if (checked(esp_timer_create(&args, &publish_timer), "publish timer") &&
      checked(esp_timer_start_periodic(publish_timer, kPublishPeriodUs), "publish timer start")) {
    ESP_LOGI(kTag, "streaming to the paired display on channel %u",
             static_cast<unsigned>(record.channel));
  }
}

void sample_user_key(void * /*argument*/) {
  const auto level = inputs.user_key_pressed() ? companion_protocol::UserKeyLevel::Pressed
                                               : companion_protocol::UserKeyLevel::Released;
  if (!long_press.sample(level)) {
    return;
  }
  (void)esp_timer_stop(user_key_timer);
  if (!write_setup_flag(true)) {
    ESP_LOGW(kTag, "could not save the display setup request");
    (void)esp_timer_start_periodic(user_key_timer, kUserKeyPeriodUs);
    return;
  }
  ESP_LOGI(kTag, "long press: restarting into display setup mode");
  inputs.restart();
}

void start_long_press() noexcept {
  if (inputs.user_key_pressed == nullptr || inputs.restart == nullptr) {
    return;
  }
  const esp_timer_create_args_t args{sample_user_key, nullptr, ESP_TIMER_TASK, "display_key", true};
  if (checked(esp_timer_create(&args, &user_key_timer), "user key timer")) {
    (void)checked(esp_timer_start_periodic(user_key_timer, kUserKeyPeriodUs),
                  "user key timer start");
  }
}

// ---- Setup mode -------------------------------------------------------------

void on_pairing_failed(void * /*handler*/, esp_event_base_t /*base*/, std::int32_t /*id*/,
                       void * /*data*/) {
  const int failures = pairing_failures.fetch_add(1) + 1;
  ESP_LOGW(kTag, "display pairing attempt failed (%d of %d)", failures, kMaxPairingFailures);
  if (failures >= kMaxPairingFailures) {
    xTaskNotifyGive(setup_task);
  }
}

// Runs once per setup boot: the flag is cleared before the window opens, so a
// crash or abort here boots back into streaming mode.
void run_setup() noexcept {
  ESP_LOGI(kTag, "display setup mode: pairing window open for %u s",
           static_cast<unsigned>(kPairingWindowMs / 1000));
  const espnow_config_t defaults = ESPNOW_INIT_CONFIG_DEFAULT();
  const espnow_config_t config = pairing::setup_config(defaults, pdMS_TO_TICKS(100));
  // espnow_init aborts on internal radio errors; the cleared flag makes the
  // resulting reboot a normal one.
  if (!checked(espnow_init(&config), "ESP-NOW setup initialization") ||
      !checked(esp_event_handler_register(ESP_EVENT_ESPNOW, ESP_EVENT_ESPNOW_SEC_FAIL,
                                          on_pairing_failed, nullptr),
               "pairing failure handler") ||
      !checked(espnow_sec_responder_start(pop.data()), "pairing responder")) {
    wipe(pop.data(), pop.size());
    inputs.restart();
  }
  const bool notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPairingWindowMs)) != 0;
  if (setup_saved.load()) {
    ESP_LOGI(kTag, "display paired; restarting into streaming mode");
  } else {
    ESP_LOGI(kTag, "%s; pairing window closed",
             notified ? "too many failed attempts" : "pairing window expired");
  }
  // Let the SDK return from the key-response path before the restart.
  vTaskDelay(pdMS_TO_TICKS(100));
  inputs.restart();
}

void startup(void * /*argument*/) {
  setup_task = xTaskGetCurrentTaskHandle();
  if (!inputs.nvs_available) {
    ESP_LOGI(kTag, "NVS unavailable; display link stays off");
    vTaskDelete(nullptr);
  }
  auto record = load_record();
  if (setup_flag_set() && inputs.restart != nullptr) {
    const bool cleared = write_setup_flag(false);
    if (cleared && load_setup_code()) {
      setup_channel = record.has_value() ? record->channel : kUnpairedSetupChannel;
      if (record.has_value()) {
        wipe(record->lmk.data(), record->lmk.size());
      }
      if (start_wifi(setup_channel)) {
        run_setup(); // never returns
      }
      inputs.restart();
    }
    ESP_LOGW(kTag, "%s; display setup skipped",
             cleared ? "no valid provisioned setup code" : "could not clear the setup request");
  }
  start_long_press();
  if (!record.has_value()) {
    ESP_LOGI(kTag, "no saved display pairing; display link stays off");
  } else {
    stream(*record);
    wipe(record->lmk.data(), record->lmk.size());
  }
  vTaskDelete(nullptr);
}

} // namespace

bool start(const vehicle_signals::SignalProvider &signal_provider,
           const Inputs &start_inputs) noexcept {
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

// Overrides the SDK overlay's weak hook. Called on the SDK task after the
// authenticated key exchange and the bounded KEY_RESP send attempt, in setup
// mode only. The key is authenticated either way; a failed send is uncertain
// and the display verifies the link after its own restart (pairing v1).
extern "C" void pairing_response_complete(const std::uint8_t *peer_mac, esp_err_t result) {
  using namespace display_espnow;
  std::array<std::uint8_t, APP_KEY_LEN> key{};
  companion_protocol::PairingRecord record{};
  bool saved = false;
  if (peer_mac != nullptr && espnow_get_key(key.data()) == ESP_OK) {
    std::memcpy(record.peer.data(), peer_mac, record.peer.size());
    record.channel = setup_channel;
    std::memcpy(record.lmk.data(), key.data(), record.lmk.size());
    // Round-trip through the codec so only a record streaming can load is saved.
    auto bytes = companion_protocol::encode_pairing_record(record);
    saved = companion_protocol::decode_pairing_record(bytes.data(), bytes.size()).has_value() &&
            save_record(record);
    wipe(bytes.data(), bytes.size());
  }
  wipe(key.data(), key.size());
  wipe(record.lmk.data(), record.lmk.size());
  // The SDK also keeps the key in its own NVS namespace; only our record is used.
  (void)espnow_erase_key();
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "key response send was not confirmed (%s)", esp_err_to_name(result));
  }
  if (!saved) {
    ESP_LOGW(kTag, "could not save the display pairing");
  }
  setup_saved.store(saved);
  xTaskNotifyGive(setup_task);
}
