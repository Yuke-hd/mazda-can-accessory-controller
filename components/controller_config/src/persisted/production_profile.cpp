#include "controller_config/persisted/production_profile.hpp"

namespace controller_config::persisted {
namespace {

using action_engine::Comparison;
using action_engine::FreshnessRequirement;
using action_engine::NumericRange;
using local_argb::internal::FillDirection;
using local_argb_actions::LedEffect;

constexpr const char *kTurnStateSignal = "vehicle.turn_state";
constexpr const char *kEngineRpmSignal = "vehicle.engine_rpm";
constexpr const char *kBrakePressedSignal = "vehicle.brake_pressed";

StateRule turn_rule(const char *action, const char *choice) {
  return StateRule{action, Condition{kTurnStateSignal, Comparison::Equal, ChoiceOperand{choice}},
                   FreshnessRequirement::Fresh};
}

} // namespace

ControllerConfig production_lighting_config() {
  ControllerConfig config{};
  config.actions = {Action{"left_turn"}, Action{"right_turn"}, Action{"hazard"},
                    Action{"rpm_fill"},  Action{"red_zone"},   Action{"brake"}};
  config.rules = {
      turn_rule("left_turn", "left"),
      turn_rule("right_turn", "right"),
      turn_rule("hazard", "hazard"),
      RangeRule{"rpm_fill", kEngineRpmSignal, NumericRange{0.0F, 6500.0F}, NumericRange{0.0F, 1.0F},
                FreshnessRequirement::FreshOrUnverified},
      SampledStateRule{"red_zone",
                       Condition{kEngineRpmSignal, Comparison::Greater, NumberOperand{6000.0F}},
                       FreshnessRequirement::FreshOrUnverified, std::nullopt},
      // Brake freshness is unset; the owner-approved opt-in accepts an
      // unverified observation without promoting it to Fresh.
      StateRule{"brake", Condition{kBrakePressedSignal, Comparison::Equal, BooleanOperand{true}},
                FreshnessRequirement::FreshOrUnverified},
  };
  // The strip is mounted mirrored, so a left turn animates the right-hand
  // effect and a right turn the left-hand one.
  config.outputs = {
      LedEffectBinding{"left_turn", LedEffect::RightTurn, 100},
      LedEffectBinding{"right_turn", LedEffect::LeftTurn, 100},
      LedEffectBinding{"hazard", LedEffect::LeftTurn, 100},
      LedEffectBinding{"hazard", LedEffect::RightTurn, 100},
      LedFillBinding{"rpm_fill", LedZone{0, 100, FillDirection::CenterOut}, Rgb{0, 16, 32}, 50},
      LedSolidBinding{"red_zone", LedZone{35, 30, FillDirection::StartToEnd}, Rgb{16, 0, 0}, 150},
      LedSolidBinding{"brake", LedZone{35, 30, FillDirection::StartToEnd}, Rgb{16, 0, 0}, 200},
  };
  return config;
}

} // namespace controller_config::persisted
