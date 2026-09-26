#include "controller_config/rpm_threshold.hpp"

#include "controller_config/rpm_level_fill.hpp"

namespace controller_config {

action_engine::SampledStateRuleConfig threshold_rule(const RpmThresholdConfig &config) noexcept {
  return {{kEngineRpmSignal, action_engine::Comparison::Greater,
           action_engine::RuleOperand::number(config.threshold.rpm)},
          config.action,
          action_engine::FreshnessRequirement::FreshOrUnverified};
}

RpmThresholdStatus apply(const RpmThresholdConfig &config, local_argb_actions::LedActionSink &leds,
                         action_engine::ActionEngine &engine) noexcept {
  RpmThresholdStatus status{};
  status.binding = leds.bind(config.action, config.effect, config.priority);
  if (status.binding != local_argb_actions::BindingStatus::Ok)
    return status;
  status.rule = engine.add_sampled_state_rule(threshold_rule(config));
  return status;
}

} // namespace controller_config
