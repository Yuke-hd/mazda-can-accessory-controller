#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "controller_config/lighting_profile.hpp"

#include <string_view>

namespace {

using action_engine::ActionId;
using controller_config::LightingProfile;
using local_argb::internal::EffectPriority;
using local_argb::internal::FillDirection;
using local_argb_actions::LedEffect;

TEST_CASE("the default profile preserves the turn signal and action IDs") {
  const LightingProfile profile{};

  CHECK(profile.turn_state_signal == std::string_view{"vehicle.turn_state"});
  CHECK(profile.turn_left.choice == std::string_view{"left"});
  CHECK(profile.turn_left.action == ActionId{1});
  CHECK(profile.turn_right.choice == std::string_view{"right"});
  CHECK(profile.turn_right.action == ActionId{2});
  CHECK(profile.hazard.choice == std::string_view{"hazard"});
  CHECK(profile.hazard.action == ActionId{3});
}

TEST_CASE("the default profile preserves mirrored turn effect bindings") {
  const LightingProfile profile{};
  const auto &bindings = profile.turn_effect_bindings;

  REQUIRE(bindings.size() == 4);
  CHECK(bindings[0].action == ActionId{1});
  CHECK(bindings[0].effect == LedEffect::RightTurn);
  CHECK(bindings[0].priority == EffectPriority{100});
  CHECK(bindings[1].action == ActionId{2});
  CHECK(bindings[1].effect == LedEffect::LeftTurn);
  CHECK(bindings[1].priority == EffectPriority{100});
  CHECK(bindings[2].action == ActionId{3});
  CHECK(bindings[2].effect == LedEffect::LeftTurn);
  CHECK(bindings[2].priority == EffectPriority{100});
  CHECK(bindings[3].action == ActionId{3});
  CHECK(bindings[3].effect == LedEffect::RightTurn);
  CHECK(bindings[3].priority == EffectPriority{100});
}

TEST_CASE("the default profile preserves the RPM level fill") {
  const LightingProfile profile{};
  const auto &fill = profile.rpm_level_fill;

  CHECK(fill.action == ActionId{4});
  CHECK(fill.range.min_rpm == 0.0F);
  CHECK(fill.range.max_rpm == 6500.0F);
  CHECK(fill.fill.zone.start == 0);
  CHECK(fill.fill.zone.length == 100);
  CHECK(fill.fill.zone.direction == FillDirection::CenterOut);
  CHECK(fill.fill.color.red == 0);
  CHECK(fill.fill.color.green == 16);
  CHECK(fill.fill.color.blue == 32);
  CHECK(fill.fill.priority == EffectPriority{50});
}

TEST_CASE("the default profile preserves the RPM red zone") {
  const LightingProfile profile{};
  const auto &red_zone = profile.rpm_red_zone;

  CHECK(red_zone.action == ActionId{5});
  CHECK(red_zone.threshold.rpm == 6000.0F);
  CHECK(red_zone.effect == LedEffect::Brake);
  CHECK(red_zone.priority == EffectPriority{150});
}

TEST_CASE("the named default object has the profile's production defaults") {
  CHECK(controller_config::kDefaultLightingProfile.turn_state_signal ==
        std::string_view{"vehicle.turn_state"});
  CHECK(controller_config::kDefaultLightingProfile.rpm_level_fill.action == ActionId{4});
  CHECK(controller_config::kDefaultLightingProfile.rpm_red_zone.action == ActionId{5});
}

} // namespace
