#pragma once

#include <cstddef>
#include <cstdint>

#include "action_engine/config_status.hpp"
#include "action_engine/engine.hpp"
#include "controller_config/lighting_profile.hpp"
#include "local_argb_actions/effect_bindings.hpp"
#include "local_argb_actions/led_action_sink.hpp"

namespace controller_config {

// Setup is deliberately reported by phase so a composition root can fail off
// and retain useful diagnostics without this portable helper logging or
// owning application lifecycle.
enum class LightingProfileApplyStage : std::uint8_t {
  Complete,
  ProfileValidation,
  TurnEffectBinding,
  SinkRegistration,
  TurnRule,
  RpmLevelFill,
  RpmRedZone,
};

enum class LightingProfileValidationError : std::uint8_t {
  None,
  EmptyTurnStateSignal,
  InvalidTurnAction,
  UnboundTurnAction,
};

struct LightingProfileApplyStatus final {
  LightingProfileApplyStage stage{LightingProfileApplyStage::Complete};
  LightingProfileValidationError validation_error{LightingProfileValidationError::None};
  std::size_t index{0};
  std::size_t turn_effect_bindings_applied{0};
  std::size_t turn_rules_applied{0};
  bool rpm_level_fill_applied{false};
  bool rpm_red_zone_applied{false};
  bool sink_registered{false};
  local_argb_actions::BindingStatus binding{local_argb_actions::BindingStatus::Ok};
  action_engine::ConfigStatus engine{action_engine::ConfigStatus::Ok};

  [[nodiscard]] bool ok() const noexcept { return stage == LightingProfileApplyStage::Complete; }
};

// Applies the profile while the engine is detached. The helper owns sink
// registration: it binds all profile turn effects first, registers `leds` next, then
// adds turn rules followed by the RPM level fill and red-zone rules. This
// preserves the firmware composition order and lets the engine deliver
// commands to the LED sink in the same order as the existing application.
//
// No provider lifecycle, engine attach/detach, renderer lifecycle, logging,
// or polling is performed here. A failed operation leaves all prior setup in
// place; the returned counts and stage identify that partial setup.
[[nodiscard]] LightingProfileApplyStatus
apply_lighting_profile(const LightingProfile &profile, local_argb_actions::LedActionSink &leds,
                       action_engine::ActionEngine &engine) noexcept;

// Consistent with the feature-specific controller_config::apply() helpers.
[[nodiscard]] inline LightingProfileApplyStatus
apply(const LightingProfile &profile, local_argb_actions::LedActionSink &leds,
      action_engine::ActionEngine &engine) noexcept {
  return apply_lighting_profile(profile, leds, engine);
}

} // namespace controller_config
