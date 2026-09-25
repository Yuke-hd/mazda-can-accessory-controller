#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_engine/action.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/led_action_sink.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using action_engine::ActionCommand;
using action_engine::ActionCommandKind;
using action_engine::ActionId;
using local_argb::internal::FillDirection;
using local_argb::internal::FillFraction;
using local_argb::internal::LedZone;
using local_argb::internal::LightingCommand;
using local_argb::internal::LightingRgb;
using local_argb_actions::BindingStatus;
using local_argb_actions::FillEffect;
using local_argb_actions::LedActionSink;
using local_argb_actions::LedEffect;
using Effects = local_argb_actions::LedEffects;

constexpr ActionId kTurnLeft{1};
constexpr ActionId kTurnRight{2};
constexpr ActionId kHazard{3};
constexpr ActionId kGauge{5};
constexpr ActionId kUnbound{9};
constexpr auto kHeld = std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();

class RecordingLightingSink final : public local_argb::internal::LightingSink {
public:
  bool publish(const LightingCommand &command) noexcept override {
    commands.push_back(command);
    return accept;
  }

  std::vector<LightingCommand> commands{};
  bool accept{true};
};

ActionCommand activate(ActionId action) { return {action, ActionCommandKind::Activate}; }
ActionCommand deactivate(ActionId action) { return {action, ActionCommandKind::Deactivate}; }
ActionCommand trigger(ActionId action) { return {action, ActionCommandKind::Trigger}; }
ActionCommand set_level(ActionId action, float level) {
  return {action, ActionCommandKind::SetLevel, level};
}

void check_effects(const LightingCommand &command, const Effects expected) {
  CHECK(command.left_turn == expected.left_turn);
  CHECK(command.right_turn == expected.right_turn);
  CHECK(command.brake == expected.brake);
}

void check_held(const LightingCommand &command, const Effects expected) {
  check_effects(command, expected);
  CHECK(command.actionable);
  CHECK(command.valid_until_us == kHeld);
}

void check_off(const LightingCommand &command) {
  check_effects(command, Effects{});
  CHECK(command.fills.empty());
  CHECK_FALSE(command.actionable);
}

constexpr LedZone kGaugeZone{20, 60, FillDirection::StartToEnd};
constexpr LightingRgb kGaugeColor{0, 0, 16};

// The single fill a command carries; fails the test when there is not exactly one.
FillFraction published_level(const LightingCommand &command) {
  REQUIRE(command.fills.size() == 1);
  const auto &fill = *command.fills.begin();
  CHECK(fill.zone == kGaugeZone);
  CHECK(fill.color.blue == kGaugeColor.blue);
  CHECK(command.actionable);
  CHECK(command.valid_until_us == kHeld);
  return fill.level;
}

// A gauge-style wiring: one action drives a level-capable fill zone.
struct GaugeSink final {
  GaugeSink() {
    REQUIRE(sink.bind(kGauge, FillEffect{kGaugeZone, kGaugeColor}) == BindingStatus::Ok);
  }

  [[nodiscard]] const LightingCommand &last() const {
    REQUIRE_FALSE(lighting.commands.empty());
    return lighting.commands.back();
  }

  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};
};

// Turn-signal wiring used by most cases: each turn action lights its own side
// and hazard lights both.
struct TurnSink final {
  TurnSink() {
    REQUIRE(sink.bind(kTurnLeft, LedEffect::LeftTurn) == BindingStatus::Ok);
    REQUIRE(sink.bind(kTurnRight, LedEffect::RightTurn) == BindingStatus::Ok);
    REQUIRE(sink.bind(kHazard, LedEffect::LeftTurn) == BindingStatus::Ok);
    REQUIRE(sink.bind(kHazard, LedEffect::RightTurn) == BindingStatus::Ok);
  }

  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};
};

} // namespace

TEST_CASE("activating a bound action publishes its effect as a held level") {
  TurnSink fixture{};

  fixture.sink.execute(activate(kTurnLeft));

  REQUIRE(fixture.lighting.commands.size() == 1);
  check_held(fixture.lighting.commands.back(), Effects{true, false, false});
}

TEST_CASE("deactivating the only active action publishes a non-actionable black command") {
  TurnSink fixture{};
  fixture.sink.execute(activate(kTurnRight));

  fixture.sink.execute(deactivate(kTurnRight));

  REQUIRE(fixture.lighting.commands.size() == 2);
  check_off(fixture.lighting.commands.back());
}

TEST_CASE("an initial Deactivate publishes an explicit black baseline") {
  TurnSink fixture{};

  fixture.sink.execute(deactivate(kTurnLeft));

  REQUIRE(fixture.lighting.commands.size() == 1);
  check_off(fixture.lighting.commands.back());
}

TEST_CASE("commands for unbound actions are ignored") {
  TurnSink fixture{};

  fixture.sink.execute(activate(kUnbound));
  fixture.sink.execute(deactivate(kUnbound));

  CHECK(fixture.lighting.commands.empty());
}

TEST_CASE("a Trigger is ignored because LED effects are levels") {
  TurnSink fixture{};
  fixture.sink.execute(activate(kTurnLeft));

  fixture.sink.execute(trigger(kTurnLeft));
  fixture.sink.execute(trigger(kTurnRight));

  REQUIRE(fixture.lighting.commands.size() == 1);
  check_held(fixture.lighting.commands.back(), Effects{true, false, false});
}

TEST_CASE("a SetLevel is ignored by on/off effects") {
  TurnSink fixture{};
  fixture.sink.execute(activate(kTurnLeft));

  fixture.sink.execute(set_level(kTurnLeft, 0.0F));
  fixture.sink.execute(set_level(kTurnRight, 1.0F));

  REQUIRE(fixture.lighting.commands.size() == 1);
  check_held(fixture.lighting.commands.back(), Effects{true, false, false});
}

TEST_CASE("one action bound to two effects lights both") {
  TurnSink fixture{};

  fixture.sink.execute(activate(kHazard));

  REQUIRE(fixture.lighting.commands.size() == 1);
  check_held(fixture.lighting.commands.back(), Effects{true, true, false});
}

TEST_CASE("an effect stays lit while any of its bound actions is active") {
  TurnSink fixture{};
  fixture.sink.execute(activate(kTurnLeft));
  fixture.sink.execute(activate(kHazard));

  fixture.sink.execute(deactivate(kHazard));
  check_held(fixture.lighting.commands.back(), Effects{true, false, false});

  fixture.sink.execute(deactivate(kTurnLeft));
  check_off(fixture.lighting.commands.back());
}

TEST_CASE("the brake effect is independent of the turn effects") {
  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};
  constexpr ActionId kBraking{4};
  REQUIRE(sink.bind(kBraking, LedEffect::Brake) == BindingStatus::Ok);
  REQUIRE(sink.bind(kTurnLeft, LedEffect::LeftTurn) == BindingStatus::Ok);

  sink.execute(activate(kBraking));
  check_held(lighting.commands.back(), Effects{false, false, true});

  sink.execute(activate(kTurnLeft));
  check_held(lighting.commands.back(), Effects{true, false, true});
}

TEST_CASE("a rejected publish is not retried; the next command carries the full state") {
  TurnSink fixture{};
  fixture.lighting.accept = false;
  fixture.sink.execute(activate(kTurnLeft));
  fixture.lighting.accept = true;

  fixture.sink.execute(activate(kTurnRight));

  REQUIRE(fixture.lighting.commands.size() == 2);
  check_held(fixture.lighting.commands.back(), Effects{true, true, false});
}

TEST_CASE("bind rejects the invalid action id zero") {
  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};

  CHECK(sink.bind(ActionId{}, LedEffect::LeftTurn) == BindingStatus::InvalidAction);
}

TEST_CASE("bind rejects the same action and effect twice") {
  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};
  REQUIRE(sink.bind(kTurnLeft, LedEffect::LeftTurn) == BindingStatus::Ok);

  CHECK(sink.bind(kTurnLeft, LedEffect::LeftTurn) == BindingStatus::DuplicateBinding);
}

TEST_CASE("bind rejects bindings beyond the fixed capacity") {
  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};
  for (std::uint16_t action = 1; action <= LedActionSink::kMaxBindings; ++action) {
    REQUIRE(sink.bind(ActionId{action}, LedEffect::Brake) == BindingStatus::Ok);
  }

  const ActionId overflow{static_cast<std::uint16_t>(LedActionSink::kMaxBindings + 1)};
  CHECK(sink.bind(overflow, LedEffect::Brake) == BindingStatus::CapacityExceeded);
}

TEST_CASE("a SetLevel lights a bound fill zone at the commanded level") {
  const struct {
    float level;
    FillFraction expected;
  } cases[] = {
      {0.25F, FillFraction::of(1, 4)},
      {0.5F, FillFraction::of(1, 2)},
      {1.0F, FillFraction::full()},
  };
  for (const auto &item : cases) {
    CAPTURE(item.level);
    GaugeSink fixture{};

    fixture.sink.execute(set_level(kGauge, item.level));

    CHECK(published_level(fixture.last()) == item.expected);
  }
}

TEST_CASE("a SetLevel of zero publishes a lit fill as off") {
  GaugeSink fixture{};
  fixture.sink.execute(set_level(kGauge, 0.5F));

  fixture.sink.execute(set_level(kGauge, 0.0F));

  REQUIRE(fixture.lighting.commands.size() == 2);
  check_off(fixture.last());
}

TEST_CASE("an out-of-range SetLevel is clamped to the nearest end of 0.0..1.0") {
  const struct {
    float level;
    bool lit;
  } cases[] = {
      {-0.5F, false},
      {-std::numeric_limits<float>::infinity(), false},
      {-std::numeric_limits<float>::denorm_min(), false},
      {1.5F, true},
      {std::numeric_limits<float>::infinity(), true},
  };
  for (const auto &item : cases) {
    CAPTURE(item.level);
    GaugeSink fixture{};

    fixture.sink.execute(set_level(kGauge, item.level));

    if (item.lit)
      CHECK(published_level(fixture.last()) == FillFraction::full());
    else
      check_off(fixture.last());
  }
}

TEST_CASE("a NaN SetLevel fails the fill off") {
  GaugeSink fixture{};
  fixture.sink.execute(set_level(kGauge, 0.75F));

  fixture.sink.execute(set_level(kGauge, std::nanf("")));

  check_off(fixture.last());
}

TEST_CASE("Deactivate sets a fill to zero level") {
  GaugeSink fixture{};
  fixture.sink.execute(set_level(kGauge, 0.75F));

  fixture.sink.execute(deactivate(kGauge));

  check_off(fixture.last());
}

TEST_CASE("Activate lights a fill zone completely") {
  GaugeSink fixture{};

  fixture.sink.execute(activate(kGauge));

  CHECK(published_level(fixture.last()) == FillFraction::full());
}

TEST_CASE("a Trigger is ignored by fills") {
  GaugeSink fixture{};

  fixture.sink.execute(trigger(kGauge));

  CHECK(fixture.lighting.commands.empty());
}

TEST_CASE("fills and on/off effects publish their full state together") {
  GaugeSink fixture{};
  REQUIRE(fixture.sink.bind(kTurnLeft, LedEffect::LeftTurn) == BindingStatus::Ok);
  fixture.sink.execute(set_level(kGauge, 0.5F));

  fixture.sink.execute(activate(kTurnLeft));

  CHECK(published_level(fixture.last()) == FillFraction::of(1, 2));
  check_effects(fixture.last(), Effects{true, false, false});

  fixture.sink.execute(deactivate(kGauge));
  CHECK(fixture.last().fills.empty());
  check_held(fixture.last(), Effects{true, false, false});
}

TEST_CASE("one action can drive a fill and an on/off effect") {
  GaugeSink fixture{};
  REQUIRE(fixture.sink.bind(kGauge, LedEffect::Brake) == BindingStatus::Ok);

  fixture.sink.execute(activate(kGauge));
  CHECK(published_level(fixture.last()) == FillFraction::full());
  check_effects(fixture.last(), Effects{false, false, true});

  // SetLevel drives only the fill; the on/off effect keeps its level.
  fixture.sink.execute(set_level(kGauge, 0.25F));
  CHECK(published_level(fixture.last()) == FillFraction::of(1, 4));
  check_effects(fixture.last(), Effects{false, false, true});
}

TEST_CASE("fill bind rejects the invalid action id zero") {
  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};

  CHECK(sink.bind(ActionId{}, FillEffect{kGaugeZone, kGaugeColor}) == BindingStatus::InvalidAction);
}

TEST_CASE("fill bind rejects the same action and zone twice but accepts another zone") {
  GaugeSink fixture{};

  CHECK(fixture.sink.bind(kGauge, FillEffect{kGaugeZone, LightingRgb{16, 0, 0}}) ==
        BindingStatus::DuplicateBinding);
  CHECK(fixture.sink.bind(kGauge, FillEffect{LedZone{0, 10, FillDirection::CenterOut},
                                             kGaugeColor}) == BindingStatus::Ok);
}

TEST_CASE("fill bind rejects bindings beyond the fixed fill capacity") {
  RecordingLightingSink lighting{};
  LedActionSink sink{lighting};
  for (std::uint16_t action = 1; action <= LedActionSink::kMaxFillBindings; ++action) {
    REQUIRE(sink.bind(ActionId{action}, FillEffect{kGaugeZone, kGaugeColor}) == BindingStatus::Ok);
  }

  const ActionId overflow{static_cast<std::uint16_t>(LedActionSink::kMaxFillBindings + 1)};
  CHECK(sink.bind(overflow, FillEffect{kGaugeZone, kGaugeColor}) ==
        BindingStatus::CapacityExceeded);
}
