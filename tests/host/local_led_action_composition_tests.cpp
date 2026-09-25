#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Host composition proof for #11 and #29: generic provider -> ActionEngine ->
// LedActionSink -> RendererController -> PixelFrameSink. The catalog is
// make-independent, so no Mazda type is reachable from this test; the renderer
// is the production state machine behind the component's private include.
#include "action_engine/engine.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"
#include "local_argb/renderer.hpp"
#include "local_argb_actions/led_action_sink.hpp"
#include "support/fake_signal_provider.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

#include <cstdint>
#include <iterator>
#include <vector>

namespace {

using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using action_engine::NumericRange;
using action_engine::RangeRuleConfig;
using action_engine::RuleOperand;
using action_engine::SignalCondition;
using action_engine::StateRuleConfig;
using local_argb::PixelFrame;
using local_argb::internal::EffectPriority;
using local_argb::internal::FillDirection;
using local_argb::internal::LedZone;
using local_argb::internal::LightingRgb;
using local_argb_actions::BindingStatus;
using local_argb_actions::FillEffect;
using local_argb_actions::LedActionSink;
using local_argb_actions::LedEffect;
using vehicle_signals::Availability;
using vehicle_signals::SignalCapability;
using vehicle_signals::SignalEnumChoice;
using vehicle_signals::SignalId;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalNotification;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalStatus;
using vehicle_signals::SignalType;
using vehicle_signals::SignalUnit;
using vehicle_signals::SignalValue;
using vehicle_signals::ValidationStatus;

constexpr SignalId kTurnState{1};
constexpr SignalId kGaugeInput{2};
constexpr std::uint16_t kOff = 0;
constexpr std::uint16_t kLeft = 1;
constexpr std::uint16_t kRight = 2;
constexpr std::uint16_t kHazard = 3;

constexpr SignalEnumChoice kTurnChoices[] = {
    {kOff, "off"}, {kLeft, "left"}, {kRight, "right"}, {kHazard, "hazard"}};

constexpr SignalMetadata kCatalog[] = {
    {kTurnState, "lamps.turn_state", SignalType::Enum, SignalUnit::None, ValidationStatus::Observed,
     SignalCapability::Read | SignalCapability::Notify, kTurnChoices, std::size(kTurnChoices)},
    // A generic numeric input for level-driven fills; no vehicle meaning.
    {kGaugeInput, "test.gauge_input", SignalType::Number, SignalUnit::None,
     ValidationStatus::Reference, SignalCapability::Read, nullptr, 0},
};
constexpr vehicle_signals::SignalCatalogView kView{kCatalog};
static_assert(kView.well_formed());

constexpr ActionId kTurnLeftAction{1};
constexpr ActionId kTurnRightAction{2};
constexpr ActionId kHazardAction{3};
constexpr ActionId kGaugeAction{4};

class RecordingPixelSink final : public local_argb::PixelFrameSink {
public:
  bool write(const PixelFrame &frame) noexcept override {
    frames.push_back(frame);
    if (failures_remaining == 0)
      return true;
    --failures_remaining;
    return false;
  }

  std::vector<PixelFrame> frames{};
  unsigned failures_remaining{0};
};

// Stands in for the ESP-IDF queue worker: applies each command to the renderer
// at the current fake time.
class RendererLightingSink final : public local_argb::internal::LightingSink {
public:
  explicit RendererLightingSink(local_argb::internal::RendererController &renderer) noexcept
      : renderer_(&renderer) {}

  bool publish(const local_argb::internal::LightingCommand &command) noexcept override {
    return renderer_->apply(command, now_us);
  }

  vehicle_core::MonotonicTimestamp now_us{0};

private:
  local_argb::internal::RendererController *renderer_;
};

SignalNotification turn(std::uint16_t choice, Availability availability = Availability::Fresh) {
  SignalNotification notice{};
  notice.id = kTurnState;
  notice.current.value = SignalValue::enumeration(choice);
  notice.current.availability = availability;
  return notice;
}

SignalNotification turn_without_value(Availability availability) {
  SignalNotification notice{};
  notice.id = kTurnState;
  notice.current.availability = availability;
  return notice;
}

StateRuleConfig turn_rule(std::string_view choice, ActionId action,
                          FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return StateRuleConfig{
      SignalCondition{"lamps.turn_state", Comparison::Equal, RuleOperand::choice(choice)}, action,
      freshness};
}

bool left_lit(const PixelFrame &frame) {
  return local_argb::internal::frame_region_has_color(frame, 0, local_argb::kTurnLedCount);
}
bool right_lit(const PixelFrame &frame) {
  return local_argb::internal::frame_region_has_color(frame, local_argb::kRightTurnLedStart,
                                                      local_argb::kTurnLedCount);
}
bool brake_lit(const PixelFrame &frame) {
  return local_argb::internal::frame_region_has_color(frame, local_argb::kBrakeLedStart,
                                                      local_argb::kBrakeLedCount);
}

// Composition root for the host: the only code that knows the engine, the
// LED sink and the renderer together.
struct Harness final {
  explicit Harness(FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
    REQUIRE(renderer.start());
    REQUIRE(led.bind(kTurnLeftAction, LedEffect::LeftTurn) == BindingStatus::Ok);
    REQUIRE(led.bind(kTurnRightAction, LedEffect::RightTurn) == BindingStatus::Ok);
    REQUIRE(led.bind(kHazardAction, LedEffect::LeftTurn) == BindingStatus::Ok);
    REQUIRE(led.bind(kHazardAction, LedEffect::RightTurn) == BindingStatus::Ok);
    REQUIRE(engine.add_sink(led) == ConfigStatus::Ok);
    REQUIRE(engine.add_state_rule(turn_rule("left", kTurnLeftAction, freshness)) ==
            ConfigStatus::Ok);
    REQUIRE(engine.add_state_rule(turn_rule("right", kTurnRightAction, freshness)) ==
            ConfigStatus::Ok);
    REQUIRE(engine.add_state_rule(turn_rule("hazard", kHazardAction, freshness)) ==
            ConfigStatus::Ok);
    REQUIRE(engine.attach() == SignalStatus::Ok);
    provider.start();
  }
  ~Harness() {
    provider.stop();
    (void)engine.detach();
  }
  Harness(const Harness &) = delete;
  Harness &operator=(const Harness &) = delete;

  void publish_at(vehicle_core::MonotonicTimestamp now_us, SignalNotification notice) {
    lighting.now_us = now_us;
    REQUIRE(provider.publish(notice) == 1);
  }
  [[nodiscard]] const PixelFrame &frame_at(vehicle_core::MonotonicTimestamp now_us) {
    REQUIRE(renderer.tick(now_us));
    return pixels.frames.back();
  }

  RecordingPixelSink pixels{};
  local_argb::internal::RendererController renderer{pixels};
  RendererLightingSink lighting{renderer};
  LedActionSink led{lighting};
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
};

SignalNotification initial(SignalNotification notice) {
  notice.initial = true;
  return notice;
}

} // namespace

TEST_CASE("a fresh left turn renders only the left turn region") {
  Harness harness{};

  harness.publish_at(1'000, initial(turn(kLeft)));

  const PixelFrame &frame = harness.frame_at(1'000);
  CHECK(left_lit(frame));
  CHECK_FALSE(right_lit(frame));
  CHECK_FALSE(brake_lit(frame));
}

TEST_CASE("a fresh right turn renders only the right turn region") {
  Harness harness{};

  harness.publish_at(1'000, initial(turn(kRight)));

  const PixelFrame &frame = harness.frame_at(1'000);
  CHECK_FALSE(left_lit(frame));
  CHECK(right_lit(frame));
  CHECK_FALSE(brake_lit(frame));
}

TEST_CASE("hazard renders both turn regions") {
  Harness harness{};

  harness.publish_at(1'000, initial(turn(kHazard)));

  const PixelFrame &frame = harness.frame_at(1'000);
  CHECK(left_lit(frame));
  CHECK(right_lit(frame));
}

TEST_CASE("a held turn keeps animating with no command deadline") {
  Harness harness{};
  harness.publish_at(1'000, initial(turn(kLeft)));

  // Ten seconds without another notice: the level is held until Deactivate.
  CHECK(left_lit(harness.frame_at(10'001'000)));
}

TEST_CASE("an initial NoData notice keeps the strip black") {
  Harness harness{};

  harness.publish_at(1'000, initial(turn_without_value(Availability::NoData)));

  CHECK(harness.frame_at(1'000) == local_argb::kBlackFrame);
}

TEST_CASE("turn switched off returns the strip to black") {
  Harness harness{};
  harness.publish_at(1'000, initial(turn(kLeft)));
  REQUIRE(left_lit(harness.frame_at(1'000)));

  harness.publish_at(2'000, turn(kOff));

  CHECK(harness.frame_at(2'000) == local_argb::kBlackFrame);
}

TEST_CASE("Stale data fails the strip off") {
  Harness harness{};
  harness.publish_at(1'000, initial(turn(kHazard)));
  REQUIRE(left_lit(harness.frame_at(1'000)));

  harness.publish_at(300'000, turn(kHazard, Availability::Stale));

  CHECK(harness.frame_at(300'000) == local_argb::kBlackFrame);
}

TEST_CASE("Unavailable data fails the strip off and recovery relights it") {
  Harness harness{};
  harness.publish_at(1'000, initial(turn(kLeft)));
  REQUIRE(left_lit(harness.frame_at(1'000)));

  SignalNotification lost = turn_without_value(Availability::Unavailable);
  lost.became_unavailable = true;
  harness.publish_at(2'000, lost);
  CHECK(harness.frame_at(2'000) == local_argb::kBlackFrame);

  SignalNotification recovered = turn(kLeft);
  recovered.recovered = true;
  harness.publish_at(3'000, recovered);
  CHECK(left_lit(harness.frame_at(3'000)));
}

TEST_CASE("a NoData notice after data fails the strip off") {
  Harness harness{};
  harness.publish_at(1'000, initial(turn(kRight)));
  REQUIRE(right_lit(harness.frame_at(1'000)));

  harness.publish_at(2'000, turn_without_value(Availability::NoData));

  CHECK(harness.frame_at(2'000) == local_argb::kBlackFrame);
}

TEST_CASE("unverified freshness stays black under the strict Fresh rule") {
  Harness harness{};

  harness.publish_at(1'000, initial(turn(kLeft, Availability::FreshnessUnverified)));

  CHECK(harness.frame_at(1'000) == local_argb::kBlackFrame);
}

TEST_CASE("unverified freshness lights the strip when the rule accepts it") {
  Harness harness{FreshnessRequirement::FreshOrUnverified};

  harness.publish_at(1'000, initial(turn(kLeft, Availability::FreshnessUnverified)));

  CHECK(left_lit(harness.frame_at(1'000)));
}

TEST_CASE("a pixel write fault fails off and stays dark until the next level change") {
  Harness harness{};
  harness.publish_at(1'000, initial(turn(kHazard)));
  REQUIRE(left_lit(harness.frame_at(1'000)));

  // One failed animation write: the renderer writes black and latches its
  // fault. Held levels bring no refresh, so the strip stays dark.
  harness.pixels.failures_remaining = 1;
  CHECK_FALSE(harness.renderer.tick(40'000));
  REQUIRE(harness.pixels.failures_remaining == 0);
  CHECK(harness.renderer.faulted());
  CHECK(harness.pixels.frames.back() == local_argb::kBlackFrame);
  CHECK(harness.frame_at(1'000'000) == local_argb::kBlackFrame);

  // The next command is the renderer's recovery boundary.
  harness.publish_at(1'001'000, turn(kLeft));
  CHECK(left_lit(harness.frame_at(1'001'000)));
}

namespace {

constexpr LightingRgb kFillColor{0, 0, 16};
constexpr local_argb::Rgb kFillPixel{0, 0, 16};
constexpr std::size_t kFillStart = 40;
constexpr std::size_t kFillLength = 8;

// Composition root for a level-driven fill: LedActionSink -> renderer.
struct FillHarness final {
  explicit FillHarness(FillDirection direction) {
    REQUIRE(renderer.start());
    REQUIRE(led.bind(kGaugeAction, FillEffect{LedZone{kFillStart, kFillLength, direction},
                                              kFillColor}) == BindingStatus::Ok);
  }

  [[nodiscard]] const PixelFrame &frame_after(const action_engine::ActionCommand &command) {
    led.execute(command);
    REQUIRE(renderer.tick(0));
    return pixels.frames.back();
  }

  RecordingPixelSink pixels{};
  local_argb::internal::RendererController renderer{pixels};
  RendererLightingSink lighting{renderer};
  LedActionSink led{lighting};
};

action_engine::ActionCommand set_level(float level) {
  return action_engine::ActionCommand{kGaugeAction, action_engine::ActionCommandKind::SetLevel,
                                      level};
}

// A black frame with pixels [first, last] lit in the fill colour.
PixelFrame lit_between(std::size_t first, std::size_t last) {
  PixelFrame frame = local_argb::kBlackFrame;
  for (std::size_t index = first; index <= last; ++index)
    frame[index] = kFillPixel;
  return frame;
}

} // namespace

TEST_CASE("a SetLevel renders its fill in every zone direction") {
  const struct {
    FillDirection direction;
    float level;
    PixelFrame expected;
  } cases[] = {
      {FillDirection::StartToEnd, 0.0F, local_argb::kBlackFrame},
      {FillDirection::StartToEnd, 0.25F, lit_between(40, 41)},
      {FillDirection::StartToEnd, 0.5F, lit_between(40, 43)},
      {FillDirection::StartToEnd, 1.0F, lit_between(40, 47)},
      {FillDirection::EndToStart, 0.0F, local_argb::kBlackFrame},
      {FillDirection::EndToStart, 0.25F, lit_between(46, 47)},
      {FillDirection::EndToStart, 0.5F, lit_between(44, 47)},
      {FillDirection::EndToStart, 1.0F, lit_between(40, 47)},
      {FillDirection::CenterOut, 0.0F, local_argb::kBlackFrame},
      {FillDirection::CenterOut, 0.25F, lit_between(43, 44)},
      {FillDirection::CenterOut, 0.5F, lit_between(42, 45)},
      {FillDirection::CenterOut, 1.0F, lit_between(40, 47)},
  };
  for (const auto &item : cases) {
    CAPTURE(static_cast<int>(item.direction));
    CAPTURE(item.level);
    FillHarness harness{item.direction};

    CHECK(harness.frame_after(set_level(item.level)) == item.expected);
  }
}

TEST_CASE("Deactivate turns a rendered fill black") {
  FillHarness harness{FillDirection::StartToEnd};
  REQUIRE(harness.frame_after(set_level(0.5F)) == lit_between(40, 43));

  CHECK(harness.frame_after(action_engine::ActionCommand{
            kGaugeAction, action_engine::ActionCommandKind::Deactivate, 0.0F}) ==
        local_argb::kBlackFrame);
}

TEST_CASE("an engine range rule drives a fill end to end and fails off without data") {
  RecordingPixelSink pixels{};
  local_argb::internal::RendererController renderer{pixels};
  RendererLightingSink lighting{renderer};
  LedActionSink led{lighting};
  test_support::FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  REQUIRE(renderer.start());
  REQUIRE(
      led.bind(kGaugeAction, FillEffect{LedZone{kFillStart, kFillLength, FillDirection::StartToEnd},
                                        kFillColor}) == BindingStatus::Ok);
  REQUIRE(engine.add_sink(led) == ConfigStatus::Ok);
  REQUIRE(engine.add_range_rule(RangeRuleConfig{"test.gauge_input", NumericRange{0.0F, 100.0F},
                                                NumericRange{0.0F, 1.0F}, kGaugeAction}) ==
          ConfigStatus::Ok);
  REQUIRE(engine.attach() == SignalStatus::Ok);

  provider.set_reading(kGaugeInput, SignalReading{SignalValue::number(50.0F), Availability::Fresh,
                                                  ValidationStatus::Reference});
  REQUIRE(engine.sample_range_rules() == SignalStatus::Ok);
  CHECK(pixels.frames.back() == lit_between(40, 43));

  provider.set_reading(kGaugeInput, SignalReading{SignalValue::number(50.0F), Availability::Stale,
                                                  ValidationStatus::Reference});
  REQUIRE(engine.sample_range_rules() == SignalStatus::Ok);
  CHECK(pixels.frames.back() == local_argb::kBlackFrame);

  (void)engine.detach();
}

// The #30 example: a gauge over 20..79 at priority 50 and the right turn,
// 65..99, at the default priority. The turn owns its whole region while lit;
// the gauge keeps 20..64 and gets 65..79 back when the turn ends.
TEST_CASE("a higher-priority turn owns its overlap with a gauge fill") {
  RecordingPixelSink pixels{};
  local_argb::internal::RendererController renderer{pixels};
  RendererLightingSink lighting{renderer};
  LedActionSink led{lighting};
  REQUIRE(renderer.start());
  REQUIRE(led.bind(kGaugeAction, FillEffect{LedZone{20, 60, FillDirection::StartToEnd}, kFillColor,
                                            EffectPriority{50}}) == BindingStatus::Ok);
  REQUIRE(led.bind(kTurnRightAction, LedEffect::RightTurn) == BindingStatus::Ok);

  led.execute(set_level(1.0F));
  led.execute({kTurnRightAction, action_engine::ActionCommandKind::Activate});
  REQUIRE(renderer.tick(0));
  PixelFrame expected = lit_between(20, local_argb::kRightTurnLedStart - 1);
  expected[local_argb::kRightTurnLedStart] = local_argb::Rgb{128, 16, 0};
  CHECK(pixels.frames.back() == expected);

  led.execute({kTurnRightAction, action_engine::ActionCommandKind::Deactivate});
  REQUIRE(renderer.tick(0));
  CHECK(pixels.frames.back() == lit_between(20, 79));
}
