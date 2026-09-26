#pragma once

#include "action_engine/config_status.hpp"
#include "action_engine/engine.hpp"
#include "action_engine/rule_config.hpp"
#include "controller_config/engine_rpm_signal.hpp"

namespace controller_config {

// RPM threshold feature: `action` is active while engine speed is strictly
// above `threshold_rpm` (for example an "rpm_red_zone" warning above 6000 rpm)
// and inactive otherwise. The threshold belongs to this controller
// configuration; the action is an ordinary Activate/Deactivate level, so any
// output adapter can bind it, independently of the RPM level fill.
// threshold_rpm must be finite. There is no hysteresis.
struct RpmThresholdConfig {
  float threshold_rpm{6000.0F};
  action_engine::ActionId action{};
};

// The sampled state rule for `config`: kEngineRpmSignal Greater
// config.threshold_rpm, driving config.action. The rule accepts
// FreshnessUnverified readings, because the provider cannot verify RPM
// freshness; missing, stale or unavailable readings still deactivate it.
[[nodiscard]] action_engine::SampledStateRuleConfig
sampled_state_rule(const RpmThresholdConfig &config) noexcept;

// Returns a bare ConfigStatus, unlike the RPM level fill's apply(), because
// there is no output binding to report. Adds sampled_state_rule(config) to
// `engine`. Call it during setup, while the
// engine is detached; a non-finite threshold is InvalidOperand. It binds no
// output: the composition root binds config.action to its chosen adapter.
[[nodiscard]] action_engine::ConfigStatus apply(const RpmThresholdConfig &config,
                                                action_engine::ActionEngine &engine) noexcept;

} // namespace controller_config
