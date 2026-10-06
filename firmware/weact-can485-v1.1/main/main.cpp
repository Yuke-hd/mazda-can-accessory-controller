#include "action_engine/engine.hpp"
#include "board/board_config.h"
#include "companion_ble/companion_ble.hpp"
#include "companion_config/apply_check.hpp"
#include "companion_config/config_service.hpp"
#include "controller_config/factory_default.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/config_store.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/persisted/model.hpp"
#include "controller_config/timing.hpp"
#include "esp_app_desc.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb_actions/led_action_sink.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"
#include "sdkconfig.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace {
constexpr char kTag[] = "weact_can485_v11";

struct ApplicationState {
  std::uint32_t turn_notifications{0};
};

const char *availability_name(const mazda::Availability availability) noexcept {
  switch (availability) {
  case mazda::Availability::NoData:
    return "NoData";
  case mazda::Availability::Fresh:
    return "Fresh";
  case mazda::Availability::Stale:
    return "Stale";
  case mazda::Availability::FreshnessUnverified:
    return "FreshnessUnverified";
  case mazda::Availability::Unavailable:
    return "Unavailable";
  }
  return "Unknown";
}

void turn_state_changed(void *context,
                        const mazda::Notification<mazda::TurnState> &notification) noexcept {
  auto *state = static_cast<ApplicationState *>(context);
  if (state != nullptr)
    ++state->turn_notifications;

  const auto value = notification.current.value.has_value()
                         ? static_cast<unsigned>(*notification.current.value)
                         : 0U;
  ESP_LOGD(kTag,
           "typed turn notice: value=%u availability=%u(%s) initial=%d unavailable=%d recovered=%d "
           "coalesced=%d",
           value, static_cast<unsigned>(notification.current.availability),
           availability_name(notification.current.availability), notification.initial,
           notification.became_unavailable, notification.recovered, notification.coalesced);
}

bool reading_is_actionable(const mazda::Reading<float> &reading) noexcept {
  return reading.value.has_value() &&
         (reading.availability == mazda::Availability::Fresh ||
          reading.availability == mazda::Availability::FreshnessUnverified);
}

// The renderer supervisor watches this count: if the notification dispatcher
// stops completing loop passes, the strip fails off without a Deactivate.
std::uint32_t notification_dispatch_progress(const void *context) noexcept {
  return static_cast<const mazda::VehicleTelemetry *>(context)->dispatch_progress();
}

// The facade owns a 32 KiB opaque service allocation. Keep it, the provider,
// the engine, the LED sink and the typed callback context in application-owned
// storage instead of the 3.5 KiB app_main task stack. The LED sink is the
// renderer queue's only publisher. Static destructors never run on ESP-IDF.
// If teardown were ever added, it would have to stop the facade
// (telemetry.stop()) before the engine is destroyed, and then call
// local_argb::fail_off(), because a stopped facade and a destroyed engine send
// no Deactivate. The facade's lifecycle owner is the vehicle I/O startup task,
// which exits after start, so such a stop would have to run on a long-lived
// owner instead.
static ApplicationState application_state{};
class ApplicationClock final : public vehicle_core::MonotonicClock {
public:
  vehicle_core::MonotonicTimestamp now() const noexcept override {
    return static_cast<vehicle_core::MonotonicTimestamp>(esp_timer_get_time());
  }
};
static ApplicationClock application_clock{};
static local_argb_actions::LedActionSink led_actions{local_argb::internal::sink(),
                                                     application_clock};
static mazda::VehicleTelemetry telemetry{};
static mazda::MazdaSignalProvider signal_provider{telemetry};
static action_engine::ActionEngine engine{signal_provider};
static controller_config::persisted::ControllerConfig active_configuration{};
// The NVS config store. The vehicle I/O startup task creates it and then
// exits, but the companion config service keeps using it for commits and
// Revert to factory, so its owner has static storage. Null when NVS is
// unavailable.
static std::unique_ptr<controller_config::persisted::ConfigStoreBackend> config_backend{};
// The companion config ports. A commit dry-runs the upload on a scratch,
// never-attached engine and LED sink whose lighting sink discards every
// command, so it never touches the live engine or the renderer queue.
static companion_config::ScratchApplyCheck config_apply_check{signal_provider, application_clock};
static companion_config::ConfigService config_service{active_configuration, config_apply_check};

void log_configuration_diagnostic(
    const char *const prefix, const controller_config::persisted::ConfigDiagnostic &diagnostic) {
  ESP_LOGE(kTag, "%s: code=%u path=%s message=%s", prefix, static_cast<unsigned>(diagnostic.code),
           diagnostic.path.c_str(), diagnostic.message.c_str());
}

constexpr char kHardwareId[] = "weact-can485-v1.1";
static_assert(controller_config::persisted::kSchemaVersion >= 0 &&
                  controller_config::persisted::kSchemaVersion <= UINT16_MAX,
              "the companion Device info carries the schema version as u16");
static_assert(controller_config::persisted::kMaxStoredControllerConfigJsonBytes ==
                  companion_protocol::kMaxConfigBytes,
              "the companion upload limit must equal the stored config limit");

// The controlled restart after a companion commit or Revert to factory, on
// the NimBLE host task, or after a vehicle I/O startup timeout, on the main
// task. The lighting layer shuts its gate, queues black and waits a bounded
// time for its own worker to write it, so the calling task never calls the
// LED driver; the restart follows either way.
[[noreturn]] void restart_after_fail_off() noexcept {
  if (!local_argb::fail_off_for_restart(local_argb::kRestartFailOffWaitUs))
    ESP_LOGW(kTag, "black frame not confirmed before the controlled restart");
  esp_restart();
}

// Starts the optional companion BLE link. It runs on its own startup task, so
// a failure here or later only loses the companion link; CAN, telemetry, the
// LED renderer and fail-off are already running and are not affected. Not
// inlined, so the Device info value stays out of app_main's own frame. The
// vehicle I/O startup no longer runs on the main task, so this call chain may
// now set the main task's stack peak; re-measure it on the bench (ADR-0004).
__attribute__((noinline)) void start_companion_link() noexcept {
  // Both strings have static storage; start() encodes them before returning.
  companion_protocol::DeviceInfo device_info{};
  device_info.config_schema_version =
      static_cast<std::uint16_t>(controller_config::persisted::kSchemaVersion);
  device_info.firmware_version = esp_app_get_description()->version;
  device_info.hardware_id = kHardwareId;
  // Bonds need the NVS partition that the config store initialized; the
  // board samples the user key that opens the pairing window.
  companion_ble::ServiceInputs inputs{};
  inputs.pairing.bond_storage = controller_config::persisted::nvs_initialized()
                                    ? companion_protocol::BondStorage::Available
                                    : companion_protocol::BondStorage::Unavailable;
  inputs.pairing.user_key_pressed = board::user_key_pressed;
  inputs.config.boot = &config_service;
  inputs.config.committer = &config_service;
  inputs.config.reverter = &config_service;
  inputs.config.restart = restart_after_fail_off;
  // The companion only reads the provider through the generic port.
  inputs.live_signals = &signal_provider;
  if (!companion_ble::start(device_info, inputs))
    ESP_LOGW(kTag, "companion BLE not started; lighting continues without the companion link");
}

// Loads and applies one boot-time JSON configuration while the engine is
// detached. The persisted model owns every string referenced by the engine and
// remains alive for the lifetime of the firmware process.
bool configure_engine_lighting(
    controller_config::persisted::ConfigStoreBackend *const backend) noexcept {
  std::optional<controller_config::persisted::ConfigStore> store;
  if (backend != nullptr)
    store.emplace(*backend);
  auto selected = controller_config::persisted::load_boot_configuration(
      store ? &*store : nullptr, controller_config::factory_default_config_json());
  // `backend` is null or the static config_backend, which outlives the service.
  config_service.record_boot(selected, backend);
  if (!selected.ok()) {
    if (selected.override_diagnostic.has_value())
      log_configuration_diagnostic("persisted override rejected", *selected.override_diagnostic);
    if (!selected.override_storage_message.empty())
      ESP_LOGE(kTag, "persisted override read failed: %s",
               selected.override_storage_message.c_str());
    if (selected.factory_diagnostic.has_value())
      log_configuration_diagnostic("factory configuration rejected", *selected.factory_diagnostic);
    else
      ESP_LOGE(kTag, "factory configuration rejected without a diagnostic");
    return false;
  }
  if (selected.override_diagnostic.has_value())
    log_configuration_diagnostic("persisted override rejected; using factory configuration",
                                 *selected.override_diagnostic);
  if (!selected.override_storage_message.empty())
    ESP_LOGE(kTag, "persisted override read failed; using factory configuration: %s",
             selected.override_storage_message.c_str());
  if (selected.source == controller_config::persisted::ConfigurationSource::PersistedOverride)
    ESP_LOGI(kTag, "using valid persisted controller configuration override");
  else
    ESP_LOGI(kTag, "using embedded factory controller configuration");

  active_configuration = std::move(*selected.configuration);
  const auto status = controller_config::persisted::apply_controller_config(active_configuration,
                                                                            led_actions, engine);
  if (!status.ok()) {
    ESP_LOGE(kTag,
             "factory configuration setup failed: stage=%u index=%u binding=%u engine=%u "
             "validation=%u validation_index=%u",
             static_cast<unsigned>(status.stage), static_cast<unsigned>(status.index),
             static_cast<unsigned>(status.binding), static_cast<unsigned>(status.engine),
             static_cast<unsigned>(status.validation.error),
             static_cast<unsigned>(status.validation.index));
    return false;
  }
  return true;
}
} // namespace

namespace {
// Runs the vehicle I/O startup sequence: startup black, configuration, signal
// subscriptions and the CAN start, in that order. Every failure after
// local_argb::start() fails off before refusing. The task that runs it becomes
// the telemetry facade's lifecycle owner, so every facade lifecycle call
// (subscribe, attach, start) must stay in this function.
bool start_vehicle_io() noexcept {
  if (!local_argb::start()) {
    ESP_LOGE(kTag, "explicit startup LED clear failed; refusing to start CAN");
    return false;
  }

  config_backend = controller_config::persisted::make_nvs_config_store_backend();
  if (config_backend == nullptr)
    ESP_LOGW(kTag, "NVS configuration storage unavailable; using factory configuration");
  if (!configure_engine_lighting(config_backend.get())) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "engine LED action setup failed; refusing to start CAN");
    return false;
  }
  // The turn channel has two subscriber slots: the typed notice log and the
  // engine. Both register while the facade is stopped.
  const auto turn_subscription =
      telemetry.on_turn_state_changed(&turn_state_changed, &application_state);
  if (!turn_subscription.ok()) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "turn notification registration failed; refusing to start CAN");
    return false;
  }
  if (engine.attach() != vehicle_signals::SignalStatus::Ok) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "engine attachment failed; refusing to start CAN");
    return false;
  }
  if (!local_argb::watch_progress(&notification_dispatch_progress, &telemetry)) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "dispatcher progress watch failed; refusing to start CAN");
    return false;
  }

  ESP_LOGI(kTag,
           "WeAct CAN485 DevBoard V1.1 vehicle CAN mode: STRICT LISTEN-ONLY; bitrate=%lu; "
           "TX queue disabled; receive API only; telemetry facade owns acquisition",
           500UL);
  if (!telemetry.start().ok()) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "strict listen-only telemetry startup failed; refusing to continue");
    return false;
  }

  ESP_LOGI(kTag, "strict listen-only CAN acquisition started through telemetry facade");
  // Sets the Live signals telemetry-started flag. It only stores an atomic
  // flag, so it never waits and never blocks the vehicle I/O startup.
  companion_ble::mark_telemetry_started();
  return true;
}

// ESP-IDF allocates a driver's interrupt on the core that installs it, and the
// TWAI, RMT and SPI drivers offer no core option. The vehicle I/O startup
// therefore runs on a short-lived task pinned to core 1, which leaves core 0 to
// the Bluetooth controller, the NimBLE host and esp_timer (ADR-0004). Tasks
// created inside the sequence use xTaskCreate(), which is tskNO_AFFINITY in
// ESP-IDF: they do not inherit this task's pinning.
constexpr BaseType_t kVehicleIoCore = 1;
// app_main ran this sequence on 3,584 B with 388 B left at its high-water
// mark; this task keeps a wide margin and is freed once it exits.
constexpr std::uint32_t kVehicleIoStartupStackBytes = 6144;
// Healthy startup takes well under a second. The renderer's own supervisor
// already bounds LED driver hangs once it runs; this bounds the rest.
// No digit separator: the validators' comment stripper reads a ' digit
// separator as the start of a character literal and silently stops stripping
// comments from that point on, so commented-out code could satisfy its rules.
constexpr std::uint32_t kVehicleIoStartupTimeoutMs = 10000;
constexpr std::uint32_t kVehicleIoStarted = 1;
constexpr std::uint32_t kVehicleIoRefused = 2;

void vehicle_io_startup_task(void *const waiting_task) noexcept {
  const bool started = start_vehicle_io();
  // This task is pinned, so the core only restates the pinning. The interrupt
  // allocation evidence is esp_intr_dump(), enabled with
  // CONFIG_WEACT_DUMP_INTERRUPT_ALLOCATION.
  ESP_LOGI(kTag, "vehicle I/O startup %s; drivers installed from core %d",
           started ? "completed" : "refused", xPortGetCoreID());
#if defined(CONFIG_WEACT_DUMP_INTERRUPT_ALLOCATION)
  (void)esp_intr_dump(nullptr);
#endif
  ESP_LOGI(kTag, "vehicle I/O startup task stack headroom: %u bytes",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  (void)xTaskNotify(static_cast<TaskHandle_t>(waiting_task),
                    started ? kVehicleIoStarted : kVehicleIoRefused, eSetValueWithOverwrite);
  vTaskDelete(nullptr);
}

// Runs start_vehicle_io() on core 1 and waits for its result. Returns true only
// when vehicle I/O started. A refused sequence has already failed off. If no
// result arrives in time, the sequence may be blocked inside a driver or the
// facade, so app_main must not touch the LED drivers concurrently; it restarts
// through restart_after_fail_off(), which only queues black for the renderer
// worker (if the renderer started) and never calls a driver itself. The next
// boot runs safe defaults and startup black again before CAN can start.
bool start_vehicle_io_on_core1() noexcept {
  // Drop any stale notification so only the startup task's result is read.
  (void)xTaskNotifyStateClear(nullptr);
  if (xTaskCreatePinnedToCore(&vehicle_io_startup_task, "vehicle_io_start",
                              kVehicleIoStartupStackBytes, xTaskGetCurrentTaskHandle(),
                              ESP_TASK_MAIN_PRIO, nullptr, kVehicleIoCore) != pdPASS) {
    // Nothing has started: the LED drivers were never installed, so the board
    // safe defaults still hold the outputs low.
    ESP_LOGE(kTag, "vehicle I/O startup task creation failed; refusing to start CAN");
    return false;
  }
  std::uint32_t result = 0;
  if (xTaskNotifyWait(0, UINT32_MAX, &result, pdMS_TO_TICKS(kVehicleIoStartupTimeoutMs)) !=
      pdTRUE) {
    ESP_LOGE(kTag, "vehicle I/O startup gave no result within %lu ms; restarting",
             static_cast<unsigned long>(kVehicleIoStartupTimeoutMs));
    restart_after_fail_off();
  }
  return result == kVehicleIoStarted;
}
} // namespace

extern "C" void app_main(void) {
  if (!board::initialize_safe_defaults()) {
    ESP_LOGE(kTag, "board safe-default initialization failed; refusing to start");
    return;
  }
  if (!start_vehicle_io_on_core1()) {
    ESP_LOGE(kTag, "vehicle I/O startup refused; refusing to continue");
    return;
  }

  // The companion link starts only after startup black, NVS initialization and
  // CAN start, and its result never gates lighting.
  start_companion_link();
  for (;;) {
    const auto speed = telemetry.speed_kph();
    const auto engine_rpm = telemetry.engine_rpm();
    if (reading_is_actionable(speed) && reading_is_actionable(engine_rpm)) {
      ESP_LOGD(kTag, "telemetry poll: speed=%.2f kph engine_rpm=%.2f", *speed.value,
               *engine_rpm.value);
    }
    // Polled rules, such as the RPM level fill and red zone, are sampled at
    // this cadence; the engine serializes them with the turn notices.
    (void)engine.sample_polled_rules();
    // The application chooses its own observation cadence. CAN receive,
    // decoding, freshness servicing, notification dispatch, and LED updates
    // remain owned by their background service tasks.
    vTaskDelay(pdMS_TO_TICKS(controller_config::kPolledRuleSamplePeriodUs / 1'000));
  }
}
