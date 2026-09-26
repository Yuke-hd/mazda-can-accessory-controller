#pragma once

#include <optional>

#include "action_engine/config_status.hpp"
#include "action_engine/engine.hpp"
#include "action_engine/rule_config.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"

namespace controller_config {

// Engine speed, in rpm, above which the threshold action is active. At or
// below it the action is inactive. It must be finite.
struct RpmThreshold {
  float rpm{6000.0F};
};

// RPM threshold feature, such as a red-zone warning: while the engine speed is
// above `threshold`, `action` is active. The threshold belongs to this
// controller configuration, never to the LED renderer, which only sees the
// action's on/off level. apply() binds the action to the local LED `effect` at
// `priority`; the action is an ordinary ActionId, so any other output adapter
// can handle it too.
struct RpmThresholdConfig {
  RpmThreshold threshold{};
  action_engine::ActionId action{};
  local_argb_actions::LedEffect effect{local_argb_actions::LedEffect::Brake};
  local_argb::internal::EffectPriority priority{};
};

// The sampled state rule for `config`: kEngineRpmSignal Greater
// config.threshold, driving config.action with Activate on crossing above and
// Deactivate on crossing back, only on a change. Like the RPM level fill it
// accepts FreshnessUnverified readings, because the provider cannot verify RPM
// freshness; missing, stale or unavailable readings still deactivate it.
[[nodiscard]] action_engine::SampledStateRuleConfig
threshold_rule(const RpmThresholdConfig &config) noexcept;

// Outcome of apply(). `rule` is empty when the binding failed and no rule was
// added.
struct RpmThresholdStatus {
  local_argb_actions::BindingStatus binding{local_argb_actions::BindingStatus::Ok};
  std::optional<action_engine::ConfigStatus> rule{};

  [[nodiscard]] bool ok() const noexcept {
    return binding == local_argb_actions::BindingStatus::Ok &&
           rule == action_engine::ConfigStatus::Ok;
  }
};

// Binds config.effect at config.priority to config.action on `leds`, then
// adds threshold_rule(config) to `engine`. Call it during setup, while the
// engine is detached. A failed binding adds no rule; a rejected rule (for
// example InvalidOperand for a non-finite threshold, or DuplicateAction when
// another level rule already drives the action) leaves the binding in place,
// so the caller must treat any failure as fatal setup and fail the renderer
// off.
[[nodiscard]] RpmThresholdStatus apply(const RpmThresholdConfig &config,
                                       local_argb_actions::LedActionSink &leds,
                                       action_engine::ActionEngine &engine) noexcept;

} // namespace controller_config
