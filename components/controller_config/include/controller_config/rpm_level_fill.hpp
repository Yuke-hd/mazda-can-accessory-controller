#pragma once

#include <optional>

#include "action_engine/config_status.hpp"
#include "action_engine/engine.hpp"
#include "action_engine/rule_config.hpp"
#include "controller_config/engine_rpm_signal.hpp"
#include "local_argb_actions/led_action_sink.hpp"

namespace controller_config {

// Engine speed, in rpm, mapped onto the fill level: min_rpm and below is an
// empty fill, max_rpm and above a full fill, and speeds in between fill
// linearly. min_rpm must be below max_rpm, and both must be finite.
struct RpmRange {
  float min_rpm{0.0F};
  float max_rpm{6500.0F};
};

// RPM level fill feature: engine speed over `range` drives the level of the
// local LED `fill` through `action`. The range belongs to this controller
// configuration, not to the LED effect, so changing it changes the mapping
// without any renderer change.
struct RpmLevelFillConfig {
  RpmRange range{};
  action_engine::ActionId action{};
  local_argb_actions::FillEffect fill{};
};

// The polled range rule for `config`: kEngineRpmSignal over config.range onto
// 0.0..1.0, driving config.action. The rule accepts FreshnessUnverified
// readings, because the provider cannot verify RPM freshness; missing, stale
// or unavailable readings still turn the fill off.
[[nodiscard]] action_engine::RangeRuleConfig range_rule(const RpmLevelFillConfig &config) noexcept;

// Outcome of apply(). `rule` is empty when the binding failed and no rule was
// added.
struct RpmLevelFillStatus {
  local_argb_actions::BindingStatus binding{local_argb_actions::BindingStatus::Ok};
  std::optional<action_engine::ConfigStatus> rule{};

  [[nodiscard]] bool ok() const noexcept {
    return binding == local_argb_actions::BindingStatus::Ok &&
           rule == action_engine::ConfigStatus::Ok;
  }
};

// Binds config.fill to config.action on `leds`, then adds range_rule(config)
// to `engine`. Call it during setup, while the engine is detached. A failed
// binding adds no rule; a rejected rule (for example InvalidRange) leaves the
// binding in place, so the caller must treat any failure as fatal setup and
// fail the renderer off.
[[nodiscard]] RpmLevelFillStatus apply(const RpmLevelFillConfig &config,
                                       local_argb_actions::LedActionSink &leds,
                                       action_engine::ActionEngine &engine) noexcept;

} // namespace controller_config
