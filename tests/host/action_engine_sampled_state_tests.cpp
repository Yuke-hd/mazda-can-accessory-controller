#include "action_engine_fixture.hpp"

#include <limits>
#include <optional>
#include <vector>

#include "action_engine/condition.hpp"
#include "action_engine/engine.hpp"
#include "action_engine/sampled_state_rule.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"

// Sampled condition rules (#32): a Read-capable signal drives an ordinary
// Activate/Deactivate action from a condition such as "engine.rpm > 3000",
// sampled at the caller's cadence through the provider's read().

namespace {

using namespace action_engine_fixture;
using action_engine::ActionCommand;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using action_engine::NumericRange;
using action_engine::RangeRuleConfig;
using action_engine::ResolvedCondition;
using action_engine::RuleOperand;
using action_engine::SampledStateRule;
using action_engine::SampledStateRuleConfig;
using action_engine::SignalCondition;
using action_engine::StateRuleConfig;
using test_support::FakeSignalProvider;
using test_support::RecordingActionSink;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalResult;
using vehicle_signals::SignalStatus;
using Commands = std::vector<ActionCommand>;

constexpr std::uint16_t kShiftLight = 5;
constexpr std::uint16_t kTachometer = 3;
constexpr std::uint16_t kCourtesyLight = 1;
constexpr float kThreshold = 3000.0F;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

SignalReading rpm_reading(float rpm, Availability availability = Availability::Fresh) {
  return SignalReading{SignalValue::number(rpm), availability, ValidationStatus::Reference};
}
SignalResult<SignalReading> rpm_read(float rpm, Availability availability = Availability::Fresh) {
  return SignalResult<SignalReading>::success(rpm_reading(rpm, availability));
}

SampledStateRule rpm_rule(Comparison comparison,
                          FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return SampledStateRule{
      ResolvedCondition{kRpm, comparison, SignalValue::number(kThreshold), freshness},
      ActionId{kShiftLight}};
}

SampledStateRuleConfig shift_light(FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return SampledStateRuleConfig{
      SignalCondition{"engine.rpm", Comparison::Greater, RuleOperand::number(kThreshold)},
      ActionId{kShiftLight}, freshness};
}

RangeRuleConfig tachometer() {
  return RangeRuleConfig{"engine.rpm", NumericRange{0.0F, 6500.0F}, NumericRange{0.0F, 1.0F},
                         ActionId{kTachometer}};
}

std::optional<ActionCommand> none() { return std::nullopt; }

// Whether a fresh rule with `comparison` against 3000 activates for `rpm`.
bool activates(Comparison comparison, float rpm) {
  SampledStateRule rule = rpm_rule(comparison);
  return rule.on_sample(rpm_read(rpm)) == std::optional<ActionCommand>{activate(kShiftLight)};
}

// A provider, an engine over it and one recording sink.
struct Bench final {
  Bench() { REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok); }
  ~Bench() {
    provider.stop();
    if (engine.attached()) {
      (void)engine.detach();
    }
  }
  Bench(const Bench &) = delete;
  Bench &operator=(const Bench &) = delete;

  Commands sample() {
    CHECK(engine.sample_polled_rules() == SignalStatus::Ok);
    return sink.take();
  }
  Commands sample_rpm(float rpm, Availability availability = Availability::Fresh) {
    provider.set_reading(kRpm, rpm_reading(rpm, availability));
    return sample();
  }

  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  RecordingActionSink sink{};
};

} // namespace

// ---------------------------------------------------------------------------
// Sampled state rule

TEST_CASE("rpm > 3000 activates above the threshold and deactivates at or below it") {
  SampledStateRule rule = rpm_rule(Comparison::Greater);
  CHECK(rule.signal() == kRpm);
  CHECK(rule.action() == ActionId{kShiftLight});
  CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
  CHECK(rule.on_sample(rpm_read(4000.0F)) == none());
  CHECK(rule.on_sample(rpm_read(3000.0F)) == deactivate(kShiftLight));
  CHECK(rule.on_sample(rpm_read(800.0F)) == none());
  CHECK(rule.on_sample(rpm_read(3001.0F)) == activate(kShiftLight));
}

TEST_CASE("the first actionable sample emits an explicit Activate or Deactivate") {
  SampledStateRule above = rpm_rule(Comparison::Greater);
  CHECK(above.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
  SampledStateRule below = rpm_rule(Comparison::Greater);
  CHECK(below.on_sample(rpm_read(900.0F)) == deactivate(kShiftLight));
  CHECK(below.on_sample(rpm_read(900.0F)) == none());
}

TEST_CASE("every comparison holds exactly on its side of the 3000 rpm threshold") {
  CHECK(activates(Comparison::Greater, 3000.5F));
  CHECK_FALSE(activates(Comparison::Greater, 3000.0F));
  CHECK(activates(Comparison::GreaterOrEqual, 3000.0F));
  CHECK_FALSE(activates(Comparison::GreaterOrEqual, 2999.5F));
  CHECK(activates(Comparison::Less, 2999.5F));
  CHECK_FALSE(activates(Comparison::Less, 3000.0F));
  CHECK(activates(Comparison::LessOrEqual, 3000.0F));
  CHECK_FALSE(activates(Comparison::LessOrEqual, 3000.5F));
  CHECK(activates(Comparison::Equal, 3000.0F));
  CHECK_FALSE(activates(Comparison::Equal, 3000.5F));
  CHECK(activates(Comparison::NotEqual, 3000.5F));
  CHECK_FALSE(activates(Comparison::NotEqual, 3000.0F));
}

TEST_CASE("NoData, Stale and Unavailable rpm deactivate once and recovery re-evaluates") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    SampledStateRule rule = rpm_rule(Comparison::Greater, FreshnessRequirement::FreshOrUnverified);
    CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
    CHECK(rule.on_sample(rpm_read(3500.0F, availability)) == deactivate(kShiftLight));
    CHECK(rule.on_sample(rpm_read(3500.0F, availability)) == none());
    CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
  }
}

TEST_CASE("a first non-actionable sample emits an explicit Deactivate") {
  SampledStateRule rule = rpm_rule(Comparison::Less);
  CHECK(rule.on_sample(SignalResult<SignalReading>::success(SignalReading{})) ==
        deactivate(kShiftLight));
  CHECK(rule.on_sample(SignalResult<SignalReading>::success(SignalReading{})) == none());
  // Recovery to a false condition keeps the output unchanged.
  CHECK(rule.on_sample(rpm_read(4000.0F)) == none());
  CHECK(rule.on_sample(rpm_read(900.0F)) == activate(kShiftLight));
}

TEST_CASE("a failed rpm read deactivates and a later good read recovers") {
  SampledStateRule rule = rpm_rule(Comparison::Greater);
  CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
  CHECK(rule.on_sample(SignalResult<SignalReading>::failure(SignalStatus::Timeout)) ==
        deactivate(kShiftLight));
  CHECK(rule.on_sample(SignalResult<SignalReading>::failure(SignalStatus::Faulted)) == none());
  CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
}

TEST_CASE("unverified rpm deactivates under Fresh and activates under FreshOrUnverified") {
  SampledStateRule strict = rpm_rule(Comparison::Greater);
  SampledStateRule lenient = rpm_rule(Comparison::Greater, FreshnessRequirement::FreshOrUnverified);
  const auto unverified = rpm_read(3500.0F, Availability::FreshnessUnverified);
  CHECK(strict.on_sample(unverified) == deactivate(kShiftLight));
  CHECK(lenient.on_sample(unverified) == activate(kShiftLight));
}

TEST_CASE("non-finite and mistyped rpm readings deactivate") {
  SampledStateRule rule = rpm_rule(Comparison::Less);
  CHECK(rule.on_sample(rpm_read(900.0F)) == activate(kShiftLight));
  CHECK(rule.on_sample(rpm_read(kNaN)) == deactivate(kShiftLight));
  CHECK(rule.on_sample(rpm_read(900.0F)) == activate(kShiftLight));
  CHECK(rule.on_sample(SignalResult<SignalReading>::success(SignalReading{
            SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Reference})) ==
        deactivate(kShiftLight));
}

TEST_CASE("reset forgets the last output so an unchanged state is emitted again") {
  SampledStateRule rule = rpm_rule(Comparison::Greater);
  CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
  rule.reset();
  CHECK(rule.on_sample(rpm_read(3500.0F)) == activate(kShiftLight));
}

// ---------------------------------------------------------------------------
// Configuration

TEST_CASE("sampled condition resolution requires Read instead of Notify") {
  const auto rpm = action_engine::resolve_sampled_condition(
      kView, SignalCondition{"engine.rpm", Comparison::Greater, RuleOperand::number(3000.0F)},
      FreshnessRequirement::Fresh);
  CHECK(rpm.status == ConfigStatus::Ok);
  REQUIRE(rpm.condition.has_value());
  CHECK(rpm.condition->signal() == kRpm);

  const auto notify_only = action_engine::resolve_sampled_condition(
      kView, SignalCondition{"climate.fan_level", Comparison::Greater, RuleOperand::number(1.0F)},
      FreshnessRequirement::Fresh);
  CHECK(notify_only.status == ConfigStatus::UnsupportedCapability);
  CHECK_FALSE(notify_only.condition.has_value());
}

TEST_CASE("sampled state rule configuration errors follow the documented check order") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};

  SampledStateRuleConfig no_action = shift_light();
  no_action.action = ActionId{};
  CHECK(engine.add_sampled_state_rule(no_action) == ConfigStatus::InvalidAction);

  SampledStateRuleConfig unknown = shift_light();
  unknown.condition.signal_key = "engine.oil_pressure";
  CHECK(engine.add_sampled_state_rule(unknown) == ConfigStatus::UnknownSignal);

  SampledStateRuleConfig notify_only = shift_light();
  notify_only.condition.signal_key = "climate.fan_level";
  CHECK(engine.add_sampled_state_rule(notify_only) == ConfigStatus::UnsupportedCapability);

  SampledStateRuleConfig mistyped = shift_light();
  mistyped.condition.operand = RuleOperand::boolean(true);
  CHECK(engine.add_sampled_state_rule(mistyped) == ConfigStatus::TypeMismatch);

  SampledStateRuleConfig not_finite = shift_light();
  not_finite.condition.operand = RuleOperand::number(kNaN);
  CHECK(engine.add_sampled_state_rule(not_finite) == ConfigStatus::InvalidOperand);

  const SampledStateRuleConfig ordered_boolean{
      SignalCondition{"body.door_open", Comparison::Greater, RuleOperand::boolean(false)},
      ActionId{kShiftLight}};
  CHECK(engine.add_sampled_state_rule(ordered_boolean) == ConfigStatus::UnsupportedComparison);

  const SampledStateRuleConfig unknown_choice{
      SignalCondition{"transmission.gear", Comparison::Equal, RuleOperand::choice("sport")},
      ActionId{kShiftLight}};
  CHECK(engine.add_sampled_state_rule(unknown_choice) == ConfigStatus::UnknownChoice);

  CHECK(engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
}

TEST_CASE("sampled state, state and range rules share one level-action namespace") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  REQUIRE(engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
  CHECK(engine.add_sampled_state_rule(shift_light()) == ConfigStatus::DuplicateAction);

  RangeRuleConfig range_on_shift_light = tachometer();
  range_on_shift_light.action = ActionId{kShiftLight};
  CHECK(engine.add_range_rule(range_on_shift_light) == ConfigStatus::DuplicateAction);
  const StateRuleConfig door_on_shift_light{
      SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
      ActionId{kShiftLight}};
  CHECK(engine.add_state_rule(door_on_shift_light) == ConfigStatus::DuplicateAction);

  REQUIRE(engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  SampledStateRuleConfig on_tachometer = shift_light();
  on_tachometer.action = ActionId{kTachometer};
  CHECK(engine.add_sampled_state_rule(on_tachometer) == ConfigStatus::DuplicateAction);
}

TEST_CASE("range and sampled state rules share the polled capacity and are stopped-only") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  for (std::size_t index = 0; index < ActionEngine::kMaxPolledRules; ++index) {
    const auto action = ActionId{static_cast<std::uint16_t>(100 + index)};
    if (index % 2 == 0) {
      RangeRuleConfig range = tachometer();
      range.action = action;
      REQUIRE(engine.add_range_rule(range) == ConfigStatus::Ok);
      continue;
    }
    SampledStateRuleConfig sampled = shift_light();
    sampled.action = action;
    REQUIRE(engine.add_sampled_state_rule(sampled) == ConfigStatus::Ok);
  }
  SampledStateRuleConfig overflow = shift_light();
  overflow.action = ActionId{200};
  CHECK(engine.add_sampled_state_rule(overflow) == ConfigStatus::CapacityExceeded);

  REQUIRE(engine.attach() == SignalStatus::Ok);
  // Polled rules read on demand and need no subscription.
  CHECK(provider.subscription_count() == 0);
  SampledStateRuleConfig late = shift_light();
  late.action = ActionId{300};
  CHECK(engine.add_sampled_state_rule(late) == ConfigStatus::InvalidState);
  CHECK(engine.detach() == SignalStatus::Ok);
}

// ---------------------------------------------------------------------------
// Acceptance: fake provider -> engine -> recording sink

TEST_CASE("sampling rpm drives a shift light on and off through the engine") {
  Bench bench{};
  REQUIRE(bench.engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);

  // Before any data the provider reads NoData: explicit fail-off baseline.
  CHECK(bench.sample() == Commands{deactivate(kShiftLight)});
  CHECK(bench.sample() == Commands{});

  CHECK(bench.sample_rpm(900.0F) == Commands{});
  CHECK(bench.sample_rpm(3500.0F) == Commands{activate(kShiftLight)});
  CHECK(bench.sample_rpm(5200.0F) == Commands{});
  CHECK(bench.sample_rpm(3000.0F) == Commands{deactivate(kShiftLight)});
  CHECK(bench.sample_rpm(3500.0F) == Commands{activate(kShiftLight)});

  // Loss and recovery of data.
  CHECK(bench.sample_rpm(3500.0F, Availability::Stale) == Commands{deactivate(kShiftLight)});
  CHECK(bench.sample_rpm(3500.0F, Availability::Unavailable) == Commands{});
  CHECK(bench.sample_rpm(3500.0F) == Commands{activate(kShiftLight)});
  bench.provider.fail_reads(kRpm, SignalStatus::Timeout);
  CHECK(bench.sample() == Commands{deactivate(kShiftLight)});
  CHECK(bench.sample_rpm(3500.0F) == Commands{activate(kShiftLight)});
}

TEST_CASE("one sample drives range and sampled state rules on the same signal") {
  Bench bench{};
  REQUIRE(bench.engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);

  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F), activate(kShiftLight)});
  CHECK(bench.sample_rpm(1625.0F) ==
        Commands{set_level(kTachometer, 0.25F), deactivate(kShiftLight)});
}

TEST_CASE("unverified rpm follows each sampled state rule's freshness policy in the engine") {
  Bench bench{};
  constexpr std::uint16_t kLenientLight = 6;
  SampledStateRuleConfig lenient = shift_light(FreshnessRequirement::FreshOrUnverified);
  lenient.action = ActionId{kLenientLight};
  REQUIRE(bench.engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_sampled_state_rule(lenient) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);

  CHECK(bench.sample_rpm(3500.0F, Availability::FreshnessUnverified) ==
        Commands{deactivate(kShiftLight), activate(kLenientLight)});
  CHECK(bench.sample_rpm(3500.0F) == Commands{activate(kShiftLight)});
}

TEST_CASE("sample_polled_rules is rejected while detached and re-attaching resets rules") {
  Bench bench{};
  REQUIRE(bench.engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
  bench.provider.set_reading(kRpm, rpm_reading(3500.0F));
  CHECK(bench.engine.sample_polled_rules() == SignalStatus::InvalidState);
  CHECK(bench.sink.commands().empty());

  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.sample() == Commands{activate(kShiftLight)});
  REQUIRE(bench.engine.detach() == SignalStatus::Ok);
  CHECK(bench.engine.sample_polled_rules() == SignalStatus::InvalidState);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.sample() == Commands{activate(kShiftLight)});
}

TEST_CASE("sampling touches only polled rules while notified rules keep their own cadence") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(StateRuleConfig{
              SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
              ActionId{kCourtesyLight}}) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_sampled_state_rule(shift_light()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.provider.subscription_count() == 1);
  bench.provider.start();

  (void)bench.provider.publish(door(true));
  CHECK(bench.sink.take() == Commands{activate(kCourtesyLight)});
  CHECK(bench.sample_rpm(3500.0F) == Commands{activate(kShiftLight)});
}
