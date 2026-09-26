#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/engine.hpp"
#include "controller_config/lighting_profile_application.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::ConfigStatus;
using controller_config::LightingProfile;
using controller_config::LightingProfileApplyStage;
using controller_config::LightingProfileValidationError;
using local_argb::internal::FillFraction;
using local_argb::internal::LightingCommand;
using local_argb_actions::BindingStatus;
using local_argb_actions::LedActionSink;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr SignalId kTurnState{1};
constexpr SignalId kEngineRpm{2};
constexpr SignalEnumChoice kTurnChoices[] = {{1, "left"}, {2, "right"}, {3, "hazard"}};
constexpr SignalMetadata kCatalog[] = {
    {kTurnState, "vehicle.turn_state", SignalType::Enum, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Notify, kTurnChoices, 3},
    {kEngineRpm, "vehicle.engine_rpm", SignalType::Number, SignalUnit::RevolutionsPerMinute,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
};
constexpr vehicle_signals::SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

class RecordingLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const LightingCommand &command) noexcept override {
    commands.push_back(command);
    return true;
  }

  std::vector<LightingCommand> commands{};
};

struct Controller final {
  Controller() : leds{lighting}, engine{provider} {}

  test_support::FakeSignalProvider provider{kView};
  RecordingLightingSink lighting{};
  LedActionSink leds;
  ActionEngine engine;
};

SignalReading rpm(const float value) {
  return SignalReading{SignalValue::number(value), Availability::FreshnessUnverified,
                       ValidationStatus::Reference};
}

vehicle_signals::SignalNotification turn(const std::uint16_t value,
                                         const Availability availability = Availability::Fresh) {
  return vehicle_signals::SignalNotification{
      kTurnState,
      SignalReading{SignalValue::enumeration(value), availability, ValidationStatus::Reference},
      false,
      false,
      false,
      false};
}

} // namespace

TEST_CASE("applying the default profile configures effects, sink, strict turns and RPM rules") {
  Controller controller{};

  const auto status = controller_config::apply_lighting_profile(
      controller_config::kDefaultLightingProfile, controller.leds, controller.engine);

  REQUIRE(status.ok());
  CHECK(status.stage == LightingProfileApplyStage::Complete);
  CHECK(status.turn_effect_bindings_applied == 4);
  CHECK(status.turn_rules_applied == 3);
  CHECK(status.rpm_level_fill_applied);
  CHECK(status.rpm_red_zone_applied);
  CHECK(status.sink_registered);
  CHECK(status.binding == BindingStatus::Ok);
  CHECK(status.engine == ConfigStatus::Ok);
}

TEST_CASE("configured turn effects and RPM rules preserve production behavior and fail off") {
  Controller controller{};
  REQUIRE(controller_config::apply(controller_config::kDefaultLightingProfile, controller.leds,
                                   controller.engine)
              .ok());
  REQUIRE(controller.engine.attach() == SignalStatus::Ok);
  controller.provider.start();

  REQUIRE(controller.provider.publish(turn(1)) == 1);
  REQUIRE_FALSE(controller.lighting.commands.empty());
  CHECK(controller.lighting.commands.back().right_turn);
  CHECK_FALSE(controller.lighting.commands.back().left_turn);

  REQUIRE(controller.provider.publish(turn(1, Availability::Stale)) == 1);
  CHECK_FALSE(controller.lighting.commands.back().actionable);

  controller.provider.set_reading(kEngineRpm, rpm(3250.0F));
  REQUIRE(controller.engine.sample_polled_rules() == SignalStatus::Ok);
  REQUIRE_FALSE(controller.lighting.commands.empty());
  REQUIRE(controller.lighting.commands.back().fills.size() == 1);
  CHECK(controller.lighting.commands.back().fills.begin()->level == FillFraction::of(32768, 65536));
  CHECK_FALSE(controller.lighting.commands.back().brake);

  controller.provider.set_reading(kEngineRpm, rpm(6500.0F));
  REQUIRE(controller.engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(controller.lighting.commands.back().fills.begin()->level == FillFraction::full());
  CHECK(controller.lighting.commands.back().brake);

  controller.provider.set_reading(kEngineRpm, SignalReading{});
  REQUIRE(controller.engine.sample_polled_rules() == SignalStatus::Ok);
  CHECK(controller.lighting.commands.back().fills.empty());
  CHECK_FALSE(controller.lighting.commands.back().brake);

  controller.provider.stop();
  CHECK(controller.engine.detach() == SignalStatus::Ok);
}

TEST_CASE("a duplicate effect reports its index and preserves earlier setup") {
  Controller controller{};
  LightingProfile profile{};
  profile.turn_effect_bindings[3] = profile.turn_effect_bindings[2];

  const auto status =
      controller_config::apply_lighting_profile(profile, controller.leds, controller.engine);

  CHECK_FALSE(status.ok());
  CHECK(status.stage == LightingProfileApplyStage::TurnEffectBinding);
  CHECK(status.index == 3);
  CHECK(status.turn_effect_bindings_applied == 3);
  CHECK(status.binding == BindingStatus::DuplicateBinding);
  CHECK_FALSE(status.sink_registered);
}

TEST_CASE("an invalid RPM range reports a failed feature after earlier profile setup") {
  Controller controller{};
  LightingProfile profile{};
  profile.rpm_level_fill.range.min_rpm = 1000.0F;
  profile.rpm_level_fill.range.max_rpm = 1000.0F;

  const auto status =
      controller_config::apply_lighting_profile(profile, controller.leds, controller.engine);

  CHECK_FALSE(status.ok());
  CHECK(status.stage == LightingProfileApplyStage::RpmLevelFill);
  CHECK(status.turn_effect_bindings_applied == 4);
  CHECK(status.turn_rules_applied == 3);
  CHECK_FALSE(status.rpm_level_fill_applied);
  CHECK_FALSE(status.rpm_red_zone_applied);
  CHECK(status.sink_registered);
  CHECK(status.binding == BindingStatus::Ok);
  CHECK(status.engine == ConfigStatus::InvalidRange);
}

TEST_CASE("an invalid red-zone threshold reports the completed fill and prior setup") {
  Controller controller{};
  LightingProfile profile{};
  profile.rpm_red_zone.threshold.rpm = std::numeric_limits<float>::quiet_NaN();

  const auto status =
      controller_config::apply_lighting_profile(profile, controller.leds, controller.engine);

  CHECK_FALSE(status.ok());
  CHECK(status.stage == LightingProfileApplyStage::RpmRedZone);
  CHECK(status.turn_effect_bindings_applied == 4);
  CHECK(status.turn_rules_applied == 3);
  CHECK(status.rpm_level_fill_applied);
  CHECK_FALSE(status.rpm_red_zone_applied);
  CHECK(status.binding == BindingStatus::Ok);
  CHECK(status.engine == ConfigStatus::InvalidOperand);
}

TEST_CASE("an incoherent turn action is rejected before any setup is mutated") {
  Controller controller{};
  LightingProfile profile{};
  profile.turn_left.action = ActionId{42};

  const auto status =
      controller_config::apply_lighting_profile(profile, controller.leds, controller.engine);

  CHECK_FALSE(status.ok());
  CHECK(status.stage == LightingProfileApplyStage::ProfileValidation);
  CHECK(status.index == 0);
  CHECK(status.validation_error == LightingProfileValidationError::UnboundTurnAction);
  CHECK(status.turn_effect_bindings_applied == 0);
  CHECK(status.turn_rules_applied == 0);
  CHECK_FALSE(status.sink_registered);
}

TEST_CASE("a duplicate sink is reported with prior effect setup visible") {
  Controller controller{};
  REQUIRE(controller.engine.add_sink(controller.leds) == ConfigStatus::Ok);

  const auto status = controller_config::apply_lighting_profile(
      controller_config::kDefaultLightingProfile, controller.leds, controller.engine);

  CHECK_FALSE(status.ok());
  CHECK(status.stage == LightingProfileApplyStage::SinkRegistration);
  CHECK(status.turn_effect_bindings_applied == 4);
  CHECK(status.turn_rules_applied == 0);
  CHECK_FALSE(status.sink_registered);
  CHECK(status.engine == ConfigStatus::DuplicateSink);
}
