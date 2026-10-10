#include "controller_config/persisted/production_profile.hpp"
#include "controller_config/signals.hpp"

#include <string>

namespace controller_config::persisted {
namespace {

using action_engine::Comparison;
using action_engine::FreshnessRequirement;
using action_engine::NumericRange;
using local_argb::internal::FillDirection;
using local_argb_actions::LedEffect;

constexpr const char *kBrakePressedSignal = "vehicle.brake_pressed";

constexpr const char *kLeftTurnActionName = "left_turn";
constexpr const char *kRightTurnActionName = "right_turn";
constexpr const char *kHazardActionName = "hazard";
constexpr const char *kRpmFillActionName = "rpm_fill";
constexpr const char *kRedZoneActionName = "red_zone";
constexpr const char *kBrakeActionName = "brake";

// The legacy brake effect's region and colour. They mirror
// local_argb::kBrakeLedStart, kBrakeLedCount and kBrightnessCeiling, which this
// component cannot include; host tests pin the two in step.
constexpr LedZone kBrakeRegion{35, 30, FillDirection::StartToEnd};
constexpr Rgb kBrakeRed{16, 0, 0};

StateRule turn_rule(const char *action, const char *choice) {
  return StateRule{action,
                   Condition{std::string{controller_config::kTurnStateSignal}, Comparison::Equal,
                             ChoiceOperand{choice}},
                   FreshnessRequirement::Fresh};
}

} // namespace

ControllerConfig production_lighting_config() {
  ControllerConfig config{};
  config.actions = {Action{kLeftTurnActionName}, Action{kRightTurnActionName},
                    Action{kHazardActionName},   Action{kRpmFillActionName},
                    Action{kRedZoneActionName},  Action{kBrakeActionName}};
  config.rules = {
      turn_rule(kLeftTurnActionName, "left"),
      turn_rule(kRightTurnActionName, "right"),
      turn_rule(kHazardActionName, "hazard"),
      RangeRule{kRpmFillActionName, std::string{controller_config::kEngineRpmSignal},
                NumericRange{0.0F, 6500.0F}, NumericRange{0.0F, 1.0F},
                FreshnessRequirement::FreshOrUnverified},
      SampledStateRule{kRedZoneActionName,
                       Condition{std::string{controller_config::kEngineRpmSignal},
                                 Comparison::Greater, NumberOperand{6000.0F}},
                       FreshnessRequirement::FreshOrUnverified, std::nullopt},
      // Brake freshness is unset; the owner-approved opt-in accepts an
      // unverified observation without promoting it to Fresh.
      StateRule{kBrakeActionName,
                Condition{kBrakePressedSignal, Comparison::Equal, BooleanOperand{true}},
                FreshnessRequirement::FreshOrUnverified},
  };
  // The strip is mounted mirrored, so a left turn animates the right-hand
  // effect and a right turn the left-hand one.
  config.outputs = {
      LedEffectBinding{kLeftTurnActionName, LedEffect::RightTurn, 100},
      LedEffectBinding{kRightTurnActionName, LedEffect::LeftTurn, 100},
      LedEffectBinding{kHazardActionName, LedEffect::LeftTurn, 100},
      LedEffectBinding{kHazardActionName, LedEffect::RightTurn, 100},
      LedFillBinding{kRpmFillActionName, LedZone{0, 100, FillDirection::CenterOut}, Rgb{0, 16, 32},
                     50},
      LedSolidBinding{kRedZoneActionName, kBrakeRegion, kBrakeRed, 150},
      LedSolidBinding{kBrakeActionName, kBrakeRegion, kBrakeRed, 200},
  };
  return config;
}

} // namespace controller_config::persisted
