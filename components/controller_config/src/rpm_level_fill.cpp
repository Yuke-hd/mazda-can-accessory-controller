#include "controller_config/rpm_level_fill.hpp"

namespace controller_config {

namespace {
constexpr action_engine::NumericRange kFillLevel{0.0F, 1.0F};
} // namespace

action_engine::RangeRuleConfig range_rule(const RpmLevelFillConfig &config) noexcept {
  return {kEngineRpmSignal, action_engine::NumericRange{config.range.min_rpm, config.range.max_rpm},
          kFillLevel, config.action, action_engine::FreshnessRequirement::FreshOrUnverified};
}

RpmLevelFillStatus apply(const RpmLevelFillConfig &config, local_argb_actions::LedActionSink &leds,
                         action_engine::ActionEngine &engine) noexcept {
  RpmLevelFillStatus status{};
  status.binding = leds.bind(config.action, config.fill);
  if (status.binding != local_argb_actions::BindingStatus::Ok)
    return status;
  status.rule = engine.add_range_rule(range_rule(config));
  return status;
}

} // namespace controller_config
