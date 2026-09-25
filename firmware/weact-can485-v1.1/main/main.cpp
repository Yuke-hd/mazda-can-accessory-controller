#include "action_engine/engine.hpp"
#include "board/board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb_actions/led_action_sink.hpp"
#include "mazda/signal_provider.hpp"
#include "mazda/vehicle_telemetry.hpp"

#include <cstdint>
#include <string_view>

namespace {
constexpr char kTag[] = "weact_can485_v11";

// Local strip lighting runs on the generic engine path:
// MazdaSignalProvider -> ActionEngine -> LedActionSink -> renderer queue.
// The strip is mounted mirrored, so, as in the retired telemetry binding, the
// vehicle's left indicator lights the renderer's right_turn region and the
// right indicator its left_turn region; hazard lights both. Brake is not bound;
// see docs/development/local-led-actions.md#firmware-composition.
constexpr std::string_view kTurnStateSignal{"vehicle.turn_state"};
constexpr action_engine::ActionId kTurnLeftAction{1};
constexpr action_engine::ActionId kTurnRightAction{2};
constexpr action_engine::ActionId kHazardAction{3};

struct TurnRule {
  std::string_view choice;
  action_engine::ActionId action;
};
constexpr TurnRule kTurnRules[] = {
    {"left", kTurnLeftAction}, {"right", kTurnRightAction}, {"hazard", kHazardAction}};

struct EffectBinding {
  action_engine::ActionId action;
  local_argb_actions::LedEffect effect;
};
constexpr EffectBinding kEffectBindings[] = {
    {kTurnLeftAction, local_argb_actions::LedEffect::RightTurn},
    {kTurnRightAction, local_argb_actions::LedEffect::LeftTurn},
    {kHazardAction, local_argb_actions::LedEffect::LeftTurn},
    {kHazardAction, local_argb_actions::LedEffect::RightTurn},
};

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

// Binds the LED effects and adds the LED sink and the turn-state rules. The
// strict Fresh requirement fails off once the turn state goes Stale after its
// 250 ms freshness timeout.
bool configure_engine_lighting() noexcept {
  for (const auto &binding : kEffectBindings) {
    const auto status = led_actions.bind(binding.action, binding.effect);
    if (status != local_argb_actions::BindingStatus::Ok) {
      ESP_LOGE(kTag, "LED effect bind failed for action %u: status=%u",
               static_cast<unsigned>(binding.action.value()), static_cast<unsigned>(status));
      return false;
    }
  }
  const auto sink_status = engine.add_sink(led_actions);
  if (sink_status != action_engine::ConfigStatus::Ok) {
    ESP_LOGE(kTag, "engine add_sink failed for the LED action sink: status=%u",
             static_cast<unsigned>(sink_status));
    return false;
  }
  for (const auto &rule : kTurnRules) {
    const action_engine::StateRuleConfig config{{kTurnStateSignal, action_engine::Comparison::Equal,
                                                 action_engine::RuleOperand::choice(rule.choice)},
                                                rule.action,
                                                action_engine::FreshnessRequirement::Fresh};
    const auto status = engine.add_state_rule(config);
    if (status != action_engine::ConfigStatus::Ok) {
      ESP_LOGE(kTag, "engine add_state_rule failed for action %u: status=%u",
               static_cast<unsigned>(rule.action.value()), static_cast<unsigned>(status));
      return false;
    }
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
    // The application chooses its own observation cadence. CAN receive,
    // decoding, freshness servicing, notification dispatch, and LED updates
    // remain owned by their background service tasks.
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
