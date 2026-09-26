#include "controller_config/lighting_profile_application.hpp"

#include <array>

namespace controller_config {
namespace {

using TurnAction = TurnActionConfig;

std::array<TurnAction, 3> turn_actions(const LightingProfile &profile) noexcept {
  return {profile.turn_left, profile.turn_right, profile.hazard};
}

bool has_binding(const LightingProfile &profile, const action_engine::ActionId action) noexcept {
  for (const auto &binding : profile.turn_effect_bindings) {
    if (binding.action == action)
      return true;
  }
  return false;
}

LightingProfileApplyStatus validate(const LightingProfile &profile) noexcept {
  LightingProfileApplyStatus status{};
  if (profile.turn_state_signal.empty()) {
    status.stage = LightingProfileApplyStage::ProfileValidation;
    status.validation_error = LightingProfileValidationError::EmptyTurnStateSignal;
    return status;
  }

  const auto actions = turn_actions(profile);
  for (std::size_t index = 0; index < actions.size(); ++index) {
    if (!actions[index].action.valid()) {
      status.stage = LightingProfileApplyStage::ProfileValidation;
      status.validation_error = LightingProfileValidationError::InvalidTurnAction;
      status.index = index;
      return status;
    }
    if (!has_binding(profile, actions[index].action)) {
      status.stage = LightingProfileApplyStage::ProfileValidation;
      status.validation_error = LightingProfileValidationError::UnboundTurnAction;
      status.index = index;
      return status;
    }
  }
  return status;
}

} // namespace

LightingProfileApplyStatus apply_lighting_profile(const LightingProfile &profile,
                                                  local_argb_actions::LedActionSink &leds,
                                                  action_engine::ActionEngine &engine) noexcept {
  auto status = validate(profile);
  if (!status.ok())
    return status;

  for (std::size_t index = 0; index < profile.turn_effect_bindings.size(); ++index) {
    const auto &binding = profile.turn_effect_bindings[index];
    status.binding = leds.bind(binding.action, binding.effect, binding.priority);
    if (status.binding != local_argb_actions::BindingStatus::Ok) {
      status.stage = LightingProfileApplyStage::TurnEffectBinding;
      status.index = index;
      return status;
    }
    ++status.turn_effect_bindings_applied;
  }

  status.engine = engine.add_sink(leds);
  if (status.engine != action_engine::ConfigStatus::Ok) {
    status.stage = LightingProfileApplyStage::SinkRegistration;
    return status;
  }
  status.sink_registered = true;

  const auto actions = turn_actions(profile);
  for (std::size_t index = 0; index < actions.size(); ++index) {
    const auto &turn = actions[index];
    status.engine =
        engine.add_state_rule({{profile.turn_state_signal, action_engine::Comparison::Equal,
                                action_engine::RuleOperand::choice(turn.choice)},
                               turn.action,
                               action_engine::FreshnessRequirement::Fresh});
    if (status.engine != action_engine::ConfigStatus::Ok) {
      status.stage = LightingProfileApplyStage::TurnRule;
      status.index = index;
      return status;
    }
    ++status.turn_rules_applied;
  }

  const auto rpm_level = controller_config::apply(profile.rpm_level_fill, leds, engine);
  status.binding = rpm_level.binding;
  status.engine = rpm_level.rule.value_or(action_engine::ConfigStatus::Ok);
  if (!rpm_level.ok()) {
    status.stage = LightingProfileApplyStage::RpmLevelFill;
    return status;
  }
  status.rpm_level_fill_applied = true;

  const auto red_zone = controller_config::apply(profile.rpm_red_zone, leds, engine);
  status.binding = red_zone.binding;
  status.engine = red_zone.rule.value_or(action_engine::ConfigStatus::Ok);
  if (!red_zone.ok()) {
    status.stage = LightingProfileApplyStage::RpmRedZone;
    return status;
  }
  status.rpm_red_zone_applied = true;

  return status;
}

} // namespace controller_config
