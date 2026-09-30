#include "action_engine/engine.hpp"
#include "board/board_config.h"
#include "controller_config/factory_default.hpp"
#include "controller_config/persisted/application.hpp"
#include "controller_config/persisted/json_loader.hpp"
#include "controller_config/timing.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb_actions/led_action_sink.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"

#include <cstdint>
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
// no Deactivate.
static ApplicationState application_state{};
static local_argb_actions::LedActionSink led_actions{local_argb::internal::sink()};
static mazda::VehicleTelemetry telemetry{};
static mazda::MazdaSignalProvider signal_provider{telemetry};
static action_engine::ActionEngine engine{signal_provider};
static controller_config::persisted::ControllerConfig factory_configuration{};

// Loads and applies the embedded canonical JSON while the engine is detached.
// The persisted model owns every string referenced by the engine and remains
// alive for the lifetime of the firmware process.
bool configure_engine_lighting() noexcept {
  const auto loaded = controller_config::persisted::parse_controller_config(
      controller_config::factory_default_config_json());
  if (!loaded.ok()) {
    if (loaded.diagnostic.has_value()) {
      const auto &diagnostic = *loaded.diagnostic;
      ESP_LOGE(kTag, "factory configuration rejected: code=%u path=%s message=%s",
               static_cast<unsigned>(diagnostic.code), diagnostic.path.c_str(),
               diagnostic.message.c_str());
    } else {
      ESP_LOGE(kTag, "factory configuration rejected without a diagnostic");
    }
    return false;
  }
  factory_configuration = std::move(*loaded.configuration);
  const auto status = controller_config::persisted::apply_controller_config(factory_configuration,
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

extern "C" void app_main(void) {
  if (!board::initialize_safe_defaults()) {
    ESP_LOGE(kTag, "board safe-default initialization failed; refusing to start");
    return;
  }
  if (!local_argb::start()) {
    ESP_LOGE(kTag, "explicit startup LED clear failed; refusing to start CAN");
    return;
  }

  if (!configure_engine_lighting()) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "engine LED action setup failed; refusing to start CAN");
    return;
  }
  // The turn channel has two subscriber slots: the typed notice log and the
  // engine. Both register while the facade is stopped.
  const auto turn_subscription =
      telemetry.on_turn_state_changed(&turn_state_changed, &application_state);
  if (!turn_subscription.ok()) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "turn notification registration failed; refusing to start CAN");
    return;
  }
  if (engine.attach() != vehicle_signals::SignalStatus::Ok) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "engine attachment failed; refusing to start CAN");
    return;
  }
  if (!local_argb::watch_progress(&notification_dispatch_progress, &telemetry)) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "dispatcher progress watch failed; refusing to start CAN");
    return;
  }

  ESP_LOGI(kTag,
           "WeAct CAN485 DevBoard V1.1 vehicle CAN mode: STRICT LISTEN-ONLY; bitrate=%lu; "
           "TX queue disabled; receive API only; telemetry facade owns acquisition",
           500UL);
  if (!telemetry.start().ok()) {
    local_argb::fail_off();
    ESP_LOGE(kTag, "strict listen-only telemetry startup failed; refusing to continue");
    return;
  }

  ESP_LOGI(kTag, "strict listen-only CAN acquisition started through telemetry facade");
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
