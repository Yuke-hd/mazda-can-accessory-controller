#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "action_engine/action.hpp"
#include "controller_config/rpm_level_fill.hpp"
#include "controller_config/rpm_threshold.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/lighting_zone.hpp"
#include "local_argb_actions/effect_bindings.hpp"

namespace controller_config {

// A turn-state choice and the action it drives. The choice names are the
// portable vehicle signal values; the action identity belongs to this
// controller profile and has no meaning to the generic action engine.
struct TurnActionConfig {
  std::string_view choice{};
  action_engine::ActionId action{};
};

// One action-to-effect binding for the local LED adapter. A turn action may
// have more than one binding, as the hazard action does for the mirrored
// strip. The default priority matches local_argb's default effect priority.
struct TurnEffectBinding {
  action_engine::ActionId action{};
  local_argb_actions::LedEffect effect{local_argb_actions::LedEffect::LeftTurn};
  local_argb::internal::EffectPriority priority{};
};

// Portable production lighting configuration. It contains only value types
// from the action engine and LED sink contract; firmware lifecycle, telemetry,
// GPIO, and task ownership remain outside this profile.
struct LightingProfile final {
  std::string_view turn_state_signal{"vehicle.turn_state"};

  TurnActionConfig turn_left{"left", action_engine::ActionId{1}};
  TurnActionConfig turn_right{"right", action_engine::ActionId{2}};
  TurnActionConfig hazard{"hazard", action_engine::ActionId{3}};

  std::array<TurnEffectBinding, 4> turn_effect_bindings{
      TurnEffectBinding{action_engine::ActionId{1}, local_argb_actions::LedEffect::RightTurn,
                        local_argb::internal::EffectPriority{100}},
      TurnEffectBinding{action_engine::ActionId{2}, local_argb_actions::LedEffect::LeftTurn,
                        local_argb::internal::EffectPriority{100}},
      TurnEffectBinding{action_engine::ActionId{3}, local_argb_actions::LedEffect::LeftTurn,
                        local_argb::internal::EffectPriority{100}},
      TurnEffectBinding{action_engine::ActionId{3}, local_argb_actions::LedEffect::RightTurn,
                        local_argb::internal::EffectPriority{100}}};

  RpmLevelFillConfig rpm_level_fill{
      RpmRange{0.0F, 6500.0F}, action_engine::ActionId{4},
      local_argb_actions::FillEffect{
          local_argb::internal::LedZone{0, 100, local_argb::internal::FillDirection::CenterOut},
          local_argb::internal::LightingRgb{0, 16, 32}, local_argb::internal::EffectPriority{50}}};

  RpmThresholdConfig rpm_red_zone{RpmThreshold{6000.0F}, action_engine::ActionId{5},
                                  local_argb_actions::LedEffect::Brake,
                                  local_argb::internal::EffectPriority{150}};
};

inline constexpr LightingProfile kDefaultLightingProfile{};

} // namespace controller_config
