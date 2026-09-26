#include "controller_config/rpm_threshold.hpp"

#include "controller_config/engine_rpm_signal.hpp"

namespace controller_config {

action_engine::SampledStateRuleConfig
sampled_state_rule(const RpmThresholdConfig &config) noexcept {
  return {{kEngineRpmSignal, action_engine::Comparison::Greater,
           action_engine::RuleOperand::number(config.threshold_rpm)},
          config.action,
          action_engine::FreshnessRequirement::FreshOrUnverified};
}

action_engine::ConfigStatus apply(const RpmThresholdConfig &config,
                                  action_engine::ActionEngine &engine) noexcept {
  return engine.add_sampled_state_rule(sampled_state_rule(config));
}

} // namespace controller_config
