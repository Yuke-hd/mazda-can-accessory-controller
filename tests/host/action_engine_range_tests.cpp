#include "action_engine_fixture.hpp"

#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "action_engine/engine.hpp"
#include "action_engine/range_rule.hpp"
#include "support/fake_signal_provider.hpp"
#include "support/recording_action_sink.hpp"

// Numeric range mapping (#10): 0..6500 RPM mapped to 0..1 progress, sampled
// at the caller's cadence through the provider's read().

namespace {

using namespace action_engine_fixture;
using action_engine::ActionCommand;
using action_engine::ActionEngine;
using action_engine::ActionId;
using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using action_engine::LinearMapping;
using action_engine::NumericControlPoint;
using action_engine::NumericCurveView;
using action_engine::NumericRange;
using action_engine::RangeRule;
using action_engine::RangeRuleConfig;
using action_engine::RuleOperand;
using action_engine::SignalCondition;
using action_engine::StateRuleConfig;
using test_support::FakeSignalProvider;
using test_support::RecordingActionSink;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalResult;
using vehicle_signals::SignalStatus;
using Commands = std::vector<ActionCommand>;

constexpr std::uint16_t kTachometer = 3;
constexpr std::uint16_t kCourtesyLight = 1;
constexpr NumericRange kRpmRange{0.0F, 6500.0F};
constexpr NumericRange kProgress{0.0F, 1.0F};
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();

SignalReading rpm_reading(float rpm, Availability availability = Availability::Fresh) {
  return SignalReading{SignalValue::number(rpm), availability, ValidationStatus::Reference};
}
SignalResult<SignalReading> rpm_read(float rpm, Availability availability = Availability::Fresh) {
  return SignalResult<SignalReading>::success(rpm_reading(rpm, availability));
}

RangeRuleConfig tachometer(FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return RangeRuleConfig{"engine.rpm", kRpmRange, kProgress, ActionId{kTachometer}, freshness};
}

RangeRule tachometer_rule(FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return RangeRule{kRpm, LinearMapping{kRpmRange, kProgress}, ActionId{kTachometer}, freshness};
}

std::optional<ActionCommand> none() { return std::nullopt; }

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

// Delegates to a FakeSignalProvider but reads alternating rpm values from any
// thread, so sampling produces a command on every call.
class AlternatingRpmProvider final : public vehicle_signals::SignalProvider {
public:
  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept override {
    return fake.catalog();
  }
  [[nodiscard]] SignalResult<SignalReading> read(SignalId) const noexcept override {
    const bool high = (reads_.fetch_add(1, std::memory_order_relaxed) % 2U) == 0U;
    return rpm_read(high ? 6500.0F : 0.0F);
  }
  [[nodiscard]] SignalResult<vehicle_signals::SignalSubscription>
  subscribe(SignalId id, vehicle_signals::SignalCallback callback,
            void *context) noexcept override {
    return fake.subscribe(id, callback, context);
  }
  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(vehicle_signals::SignalSubscription subscription) noexcept override {
    return fake.unsubscribe(subscription);
  }

  FakeSignalProvider fake{kView};

private:
  mutable std::atomic<unsigned> reads_{0};
};

// Records whether two execute() calls ever overlap.
class OverlapDetectingSink final : public action_engine::ActionSink {
public:
  void execute(const ActionCommand &) noexcept override {
    if (in_flight_.fetch_add(1) != 0) {
      overlapped_ = true;
    }
    std::this_thread::sleep_for(std::chrono::microseconds{20});
    ++executed_;
    in_flight_.fetch_sub(1);
  }
  [[nodiscard]] bool overlapped() const noexcept { return overlapped_; }
  [[nodiscard]] unsigned executed() const noexcept { return executed_; }

private:
  std::atomic<int> in_flight_{0};
  std::atomic<bool> overlapped_{false};
  std::atomic<unsigned> executed_{0};
};

} // namespace

TEST_CASE("the fake provider reads NoData until set and rejects unreadable signals") {
  FakeSignalProvider provider{kView};
  const auto initial = provider.read(kRpm);
  REQUIRE(initial.ok());
  CHECK(initial.value->availability == Availability::NoData);
  CHECK_FALSE(initial.value->value.has_value());
  CHECK(provider.read(SignalId{99}).status == SignalStatus::InvalidSignal);
  CHECK(provider.read(kFanLevel).status == SignalStatus::UnsupportedCapability);

  provider.set_reading(kRpm, rpm_reading(900.0F));
  CHECK(provider.read(kRpm).value->value ==
        std::optional<SignalValue>{SignalValue::number(900.0F)});
  provider.fail_reads(kRpm, SignalStatus::Faulted);
  CHECK(provider.read(kRpm).status == SignalStatus::Faulted);
  provider.set_reading(kRpm, rpm_reading(900.0F));
  CHECK(provider.read(kRpm).ok());
}

// ---------------------------------------------------------------------------
// Mapping

TEST_CASE("0..6500 rpm maps to 0..1 with exact clamped endpoints") {
  constexpr LinearMapping mapping{kRpmRange, kProgress};
  CHECK(mapping.map(0.0F) == 0.0F);
  CHECK(mapping.map(6500.0F) == 1.0F);
  CHECK(mapping.map(3250.0F) == 0.5F);
  CHECK(mapping.map(1625.0F) == 0.25F);
  CHECK(mapping.map(-100.0F) == 0.0F);
  CHECK(mapping.map(8000.0F) == 1.0F);
}

TEST_CASE("a descending output range maps 0..6500 rpm onto 1..0") {
  constexpr LinearMapping inverse{kRpmRange, NumericRange{1.0F, 0.0F}};
  CHECK(inverse.map(0.0F) == 1.0F);
  CHECK(inverse.map(3250.0F) == 0.5F);
  CHECK(inverse.map(6500.0F) == 0.0F);
  CHECK(inverse.map(9000.0F) == 0.0F);
  CHECK(inverse.map(-1.0F) == 1.0F);
}

TEST_CASE("an equal output range maps every rpm to one constant level") {
  constexpr LinearMapping constant{kRpmRange, NumericRange{0.75F, 0.75F}};
  CHECK(constant.map(-1.0F) == 0.75F);
  CHECK(constant.map(3000.0F) == 0.75F);
  CHECK(constant.map(7000.0F) == 0.75F);
}

TEST_CASE("extreme values beyond a narrow input range clamp exactly instead of overflowing") {
  // (3e38 - 0) / 1e-30 overflows to infinity; clamping must not reach it.
  constexpr NumericRange narrow{0.0F, 1.0e-30F};
  CHECK(LinearMapping{narrow, kProgress}.map(3.0e38F) == 1.0F);
  CHECK(LinearMapping{narrow, kProgress}.map(-3.0e38F) == 0.0F);
  CHECK(LinearMapping{narrow, NumericRange{0.75F, 0.75F}}.map(3.0e38F) == 0.75F);
  CHECK(LinearMapping{narrow, NumericRange{0.75F, 0.75F}}.map(-3.0e38F) == 0.75F);
}

TEST_CASE("a numeric curve interpolates each segment and returns exact configured points") {
  constexpr NumericControlPoint points[] = {
      {0.0F, 0.0F}, {10.0F, 1.0F}, {30.0F, 0.5F}, {40.0F, 0.5F}};
  constexpr LinearMapping mapping{NumericCurveView{points, 4}};

  CHECK(mapping.map(-1.0F) == 0.0F);
  CHECK(mapping.map(0.0F) == 0.0F);
  CHECK(mapping.map(5.0F) == 0.5F);
  CHECK(mapping.map(10.0F) == 1.0F);
  CHECK(mapping.map(20.0F) == 0.75F);
  CHECK(mapping.map(30.0F) == 0.5F);
  CHECK(mapping.map(35.0F) == 0.5F);
  CHECK(mapping.map(40.0F) == 0.5F);
  CHECK(mapping.map(41.0F) == 0.5F);
}

TEST_CASE("a two-point curve is equivalent to the legacy linear mapping") {
  constexpr NumericControlPoint points[] = {{0.0F, 1.0F}, {6500.0F, 0.0F}};
  constexpr LinearMapping legacy{kRpmRange, NumericRange{1.0F, 0.0F}};
  constexpr LinearMapping curve{NumericCurveView{points, 2}};

  for (const float value : {-100.0F, 0.0F, 1625.0F, 3250.0F, 6500.0F, 8000.0F}) {
    CHECK(curve.map(value) == legacy.map(value));
  }
}

// ---------------------------------------------------------------------------
// Range rule sampling

TEST_CASE("a range rule emits SetLevel only when the level changes") {
  RangeRule rule = tachometer_rule();
  CHECK(rule.signal() == kRpm);
  CHECK(rule.action() == ActionId{kTachometer});
  CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
  CHECK(rule.on_sample(rpm_read(3250.0F)) == none());
  CHECK(rule.on_sample(rpm_read(6500.0F)) == set_level(kTachometer, 1.0F));
  // Different inputs clamped to the same level are deduplicated.
  CHECK(rule.on_sample(rpm_read(8000.0F)) == none());
  CHECK(rule.on_sample(rpm_read(0.0F)) == set_level(kTachometer, 0.0F));
}

TEST_CASE("NoData, Stale and Unavailable rpm each fail off once") {
  for (const auto availability :
       {Availability::NoData, Availability::Stale, Availability::Unavailable}) {
    RangeRule rule = tachometer_rule(FreshnessRequirement::FreshOrUnverified);
    CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
    CHECK(rule.on_sample(rpm_read(3250.0F, availability)) == deactivate(kTachometer));
    CHECK(rule.on_sample(rpm_read(3250.0F, availability)) == none());
    CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
  }
}

TEST_CASE("a first non-actionable sample emits an explicit Deactivate") {
  RangeRule rule = tachometer_rule();
  CHECK(rule.on_sample(SignalResult<SignalReading>::success(SignalReading{})) ==
        deactivate(kTachometer));
  CHECK(rule.on_sample(SignalResult<SignalReading>::success(SignalReading{})) == none());
}

TEST_CASE("a failed rpm read fails off and a later good read recovers") {
  RangeRule rule = tachometer_rule();
  CHECK(rule.on_sample(rpm_read(1625.0F)) == set_level(kTachometer, 0.25F));
  CHECK(rule.on_sample(SignalResult<SignalReading>::failure(SignalStatus::Timeout)) ==
        deactivate(kTachometer));
  CHECK(rule.on_sample(SignalResult<SignalReading>::failure(SignalStatus::Faulted)) == none());
  CHECK(rule.on_sample(rpm_read(1625.0F)) == set_level(kTachometer, 0.25F));
}

TEST_CASE("unverified rpm fails off under Fresh and maps under FreshOrUnverified") {
  RangeRule strict = tachometer_rule();
  RangeRule lenient = tachometer_rule(FreshnessRequirement::FreshOrUnverified);
  const auto unverified = rpm_read(3250.0F, Availability::FreshnessUnverified);
  CHECK(strict.on_sample(unverified) == deactivate(kTachometer));
  CHECK(lenient.on_sample(unverified) == set_level(kTachometer, 0.5F));
}

TEST_CASE("non-finite and mistyped rpm readings fail off") {
  RangeRule rule = tachometer_rule();
  CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
  CHECK(rule.on_sample(rpm_read(kNaN)) == deactivate(kTachometer));
  CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
  CHECK(rule.on_sample(rpm_read(kInfinity)) == deactivate(kTachometer));
  CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
  CHECK(rule.on_sample(SignalResult<SignalReading>::success(SignalReading{
            SignalValue::boolean(true), Availability::Fresh, ValidationStatus::Reference})) ==
        deactivate(kTachometer));
}

TEST_CASE("reset forgets the last level so the next sample is emitted again") {
  RangeRule rule = tachometer_rule();
  CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
  rule.reset();
  CHECK(rule.on_sample(rpm_read(3250.0F)) == set_level(kTachometer, 0.5F));
}

// ---------------------------------------------------------------------------
// Configuration

TEST_CASE("range rule configuration errors follow the documented check order") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};

  RangeRuleConfig no_action = tachometer();
  no_action.action = ActionId{};
  CHECK(engine.add_range_rule(no_action) == ConfigStatus::InvalidAction);

  RangeRuleConfig unknown = tachometer();
  unknown.signal_key = "engine.oil_pressure";
  CHECK(engine.add_range_rule(unknown) == ConfigStatus::UnknownSignal);

  RangeRuleConfig notify_only = tachometer();
  notify_only.signal_key = "climate.fan_level";
  CHECK(engine.add_range_rule(notify_only) == ConfigStatus::UnsupportedCapability);

  RangeRuleConfig boolean = tachometer();
  boolean.signal_key = "body.door_open";
  CHECK(engine.add_range_rule(boolean) == ConfigStatus::TypeMismatch);
  RangeRuleConfig enumeration = tachometer();
  enumeration.signal_key = "transmission.gear";
  CHECK(engine.add_range_rule(enumeration) == ConfigStatus::TypeMismatch);

  const NumericRange invalid_ranges[] = {
      {6500.0F, 0.0F},     // descending input
      {3000.0F, 3000.0F},  // empty input
      {kNaN, 6500.0F},     // non-finite bound
      {0.0F, kInfinity},   // non-finite bound
      {-3.0e38F, 3.0e38F}, // span overflows to infinity
  };
  for (const auto &input : invalid_ranges) {
    RangeRuleConfig config = tachometer();
    config.input = input;
    CHECK(engine.add_range_rule(config) == ConfigStatus::InvalidRange);
  }
  const NumericRange invalid_outputs[] = {{0.0F, kNaN}, {-kInfinity, 1.0F}, {-3.0e38F, 3.0e38F}};
  for (const auto &output : invalid_outputs) {
    RangeRuleConfig config = tachometer();
    config.output = output;
    CHECK(engine.add_range_rule(config) == ConfigStatus::InvalidRange);
  }

  // Descending and constant outputs are valid.
  RangeRuleConfig inverse = tachometer();
  inverse.output = NumericRange{1.0F, 0.0F};
  CHECK(engine.add_range_rule(inverse) == ConfigStatus::Ok);
  RangeRuleConfig constant = tachometer();
  constant.action = ActionId{kTachometer + 1};
  constant.output = NumericRange{0.5F, 0.5F};
  CHECK(engine.add_range_rule(constant) == ConfigStatus::Ok);
}

TEST_CASE("curve validation rejects malformed control points and accepts the fixed capacity") {
  constexpr NumericControlPoint one_point[] = {{0.0F, 0.0F}};
  constexpr NumericControlPoint duplicate_input[] = {{0.0F, 0.0F}, {0.0F, 1.0F}};
  constexpr NumericControlPoint descending_input[] = {{1.0F, 0.0F}, {0.0F, 1.0F}};
  constexpr NumericControlPoint non_finite_input[] = {{0.0F, 0.0F}, {kInfinity, 1.0F}};
  constexpr NumericControlPoint non_finite_output[] = {{0.0F, 0.0F}, {1.0F, kNaN}};
  constexpr NumericControlPoint overflowing_input_span[] = {{-3.0e38F, 0.0F}, {3.0e38F, 1.0F}};
  constexpr NumericControlPoint overflowing_output_span[] = {{0.0F, -3.0e38F}, {1.0F, 3.0e38F}};
  constexpr NumericControlPoint maximum[] = {
      {0.0F, 0.0F}, {1.0F, 0.125F}, {2.0F, 0.25F}, {3.0F, 0.375F},
      {4.0F, 0.5F}, {5.0F, 0.625F}, {6.0F, 0.75F}, {7.0F, 0.875F},
  };
  constexpr NumericControlPoint over_capacity[] = {{0.0F, 0.0F}, {1.0F, 0.1F}, {2.0F, 0.2F},
                                                   {3.0F, 0.3F}, {4.0F, 0.4F}, {5.0F, 0.5F},
                                                   {6.0F, 0.6F}, {7.0F, 0.7F}, {8.0F, 0.8F}};

  const NumericCurveView invalid[] = {
      {},
      {nullptr, 2},
      {one_point, 1},
      {duplicate_input, 2},
      {descending_input, 2},
      {non_finite_input, 2},
      {non_finite_output, 2},
      {overflowing_input_span, 2},
      {overflowing_output_span, 2},
      {over_capacity, NumericCurveView::kMaxPoints + 1},
  };
  for (const NumericCurveView curve : invalid) {
    RangeRuleConfig config = tachometer();
    config.curve = curve;
    CHECK(resolve_range_rule(kView, config).status == ConfigStatus::InvalidRange);
  }

  static_assert(NumericCurveView::kMaxPoints == 8);
  RangeRuleConfig at_capacity = tachometer();
  at_capacity.curve = NumericCurveView{maximum, NumericCurveView::kMaxPoints};
  auto resolution = resolve_range_rule(kView, at_capacity);
  REQUIRE(resolution.status == ConfigStatus::Ok);
  REQUIRE(resolution.rule.has_value());
  CHECK(resolution.rule->on_sample(rpm_read(0.0F)) == set_level(kTachometer, 0.0F));
  CHECK(resolution.rule->on_sample(rpm_read(3.5F)) == set_level(kTachometer, 0.4375F));
  CHECK(resolution.rule->on_sample(rpm_read(7.0F)) == set_level(kTachometer, 0.875F));
}

TEST_CASE("a present curve takes precedence over legacy ranges") {
  constexpr NumericControlPoint points[] = {{0.0F, 0.0F}, {10.0F, 1.0F}, {20.0F, 0.25F}};
  RangeRuleConfig config = tachometer();
  config.input = NumericRange{kNaN, kNaN};
  config.output = NumericRange{kInfinity, kInfinity};
  config.curve = NumericCurveView{points, 3};

  auto resolution = resolve_range_rule(kView, config);
  REQUIRE(resolution.status == ConfigStatus::Ok);
  REQUIRE(resolution.rule.has_value());
  CHECK(resolution.rule->on_sample(rpm_read(15.0F)) == set_level(kTachometer, 0.625F));
}

TEST_CASE("a resolved curve owns a bounded copy of borrowed control points") {
  NumericControlPoint points[] = {{0.0F, 0.0F}, {10.0F, 1.0F}, {20.0F, 0.0F}};
  RangeRuleConfig config = tachometer();
  config.curve = NumericCurveView{points, 3};
  auto resolution = resolve_range_rule(kView, config);
  REQUIRE(resolution.status == ConfigStatus::Ok);
  REQUIRE(resolution.rule.has_value());

  points[1] = NumericControlPoint{10.0F, 0.25F};
  points[2] = NumericControlPoint{20.0F, 1.0F};

  CHECK(resolution.rule->on_sample(rpm_read(10.0F)) == set_level(kTachometer, 1.0F));
  CHECK(resolution.rule->on_sample(rpm_read(15.0F)) == set_level(kTachometer, 0.5F));
}

TEST_CASE("range and state rules share one level-action namespace") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  REQUIRE(engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  CHECK(engine.add_range_rule(tachometer()) == ConfigStatus::DuplicateAction);

  const StateRuleConfig door_on_tachometer{
      SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
      ActionId{kTachometer}};
  CHECK(engine.add_state_rule(door_on_tachometer) == ConfigStatus::DuplicateAction);

  const StateRuleConfig courtesy{
      SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
      ActionId{kCourtesyLight}};
  REQUIRE(engine.add_state_rule(courtesy) == ConfigStatus::Ok);
  RangeRuleConfig rpm_on_light = tachometer();
  rpm_on_light.action = ActionId{kCourtesyLight};
  CHECK(engine.add_range_rule(rpm_on_light) == ConfigStatus::DuplicateAction);
}

TEST_CASE("the ninth range rule exceeds capacity and range rules are stopped-only") {
  FakeSignalProvider provider{kView};
  ActionEngine engine{provider};
  for (std::size_t index = 0; index < ActionEngine::kMaxPolledRules; ++index) {
    RangeRuleConfig config = tachometer();
    config.action = ActionId{static_cast<std::uint16_t>(100 + index)};
    REQUIRE(engine.add_range_rule(config) == ConfigStatus::Ok);
  }
  RangeRuleConfig ninth = tachometer();
  ninth.action = ActionId{200};
  CHECK(engine.add_range_rule(ninth) == ConfigStatus::CapacityExceeded);

  REQUIRE(engine.attach() == SignalStatus::Ok);
  // Range rules read on demand and need no subscription.
  CHECK(provider.subscription_count() == 0);
  RangeRuleConfig late = tachometer();
  late.action = ActionId{300};
  CHECK(engine.add_range_rule(late) == ConfigStatus::InvalidState);
  CHECK(engine.detach() == SignalStatus::Ok);
}

// ---------------------------------------------------------------------------
// Acceptance: fake provider -> engine -> recording sink

TEST_CASE("sampling 0..6500 rpm drives tachometer progress through the engine") {
  Bench bench{};
  REQUIRE(bench.engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);

  // Before any data the provider reads NoData: explicit fail-off baseline.
  CHECK(bench.sample() == Commands{deactivate(kTachometer)});
  CHECK(bench.sample() == Commands{});

  CHECK(bench.sample_rpm(0.0F) == Commands{set_level(kTachometer, 0.0F)});
  CHECK(bench.sample_rpm(6500.0F) == Commands{set_level(kTachometer, 1.0F)});
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
  CHECK(bench.sample_rpm(3250.0F) == Commands{});
  CHECK(bench.sample_rpm(-100.0F) == Commands{set_level(kTachometer, 0.0F)});
  CHECK(bench.sample_rpm(8000.0F) == Commands{set_level(kTachometer, 1.0F)});

  // Loss and recovery of data.
  CHECK(bench.sample_rpm(8000.0F, Availability::Stale) == Commands{deactivate(kTachometer)});
  CHECK(bench.sample_rpm(8000.0F, Availability::Unavailable) == Commands{});
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
  bench.provider.set_reading(kRpm, SignalReading{});
  CHECK(bench.sample() == Commands{deactivate(kTachometer)});
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
  bench.provider.fail_reads(kRpm, SignalStatus::Timeout);
  CHECK(bench.sample() == Commands{deactivate(kTachometer)});
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
}

TEST_CASE("unverified rpm follows each range rule's freshness policy in the engine") {
  Bench bench{};
  constexpr std::uint16_t kLenientGauge = 4;
  RangeRuleConfig lenient = tachometer(FreshnessRequirement::FreshOrUnverified);
  lenient.action = ActionId{kLenientGauge};
  REQUIRE(bench.engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_range_rule(lenient) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);

  CHECK(bench.sample_rpm(3250.0F, Availability::FreshnessUnverified) ==
        Commands{deactivate(kTachometer), set_level(kLenientGauge, 0.5F)});
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
}

TEST_CASE("sample_polled_rules is rejected while detached") {
  Bench bench{};
  REQUIRE(bench.engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  bench.provider.set_reading(kRpm, rpm_reading(3250.0F));
  CHECK(bench.engine.sample_polled_rules() == SignalStatus::InvalidState);
  CHECK(bench.sink.commands().empty());

  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.sample() == Commands{set_level(kTachometer, 0.5F)});
  REQUIRE(bench.engine.detach() == SignalStatus::Ok);
  CHECK(bench.engine.sample_polled_rules() == SignalStatus::InvalidState);
}

TEST_CASE("re-attaching resets range rules so an unchanged level is emitted again") {
  Bench bench{};
  REQUIRE(bench.engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
  REQUIRE(bench.engine.detach() == SignalStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.sample_rpm(3250.0F) == Commands{set_level(kTachometer, 0.5F)});
}

TEST_CASE("a door state rule and an rpm range rule coexist on one engine") {
  Bench bench{};
  REQUIRE(bench.engine.add_state_rule(StateRuleConfig{
              SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
              ActionId{kCourtesyLight}}) == ConfigStatus::Ok);
  REQUIRE(bench.engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  REQUIRE(bench.engine.attach() == SignalStatus::Ok);
  CHECK(bench.provider.subscription_count() == 1);
  bench.provider.start();

  (void)bench.provider.publish(door(true));
  CHECK(bench.sink.take() == Commands{activate(kCourtesyLight)});
  // Reads work while the provider runs; sampling touches only range rules.
  CHECK(bench.sample_rpm(1625.0F) == Commands{set_level(kTachometer, 0.25F)});
  (void)bench.provider.publish(door(false));
  CHECK(bench.sink.take() == Commands{deactivate(kCourtesyLight)});
  CHECK(bench.sample_rpm(1625.0F) == Commands{});
}

TEST_CASE("sampling on one thread and notices on another never overlap sink calls") {
  constexpr int kRounds = 200;
  AlternatingRpmProvider provider{};
  ActionEngine engine{provider};
  OverlapDetectingSink sink{};
  REQUIRE(engine.add_sink(sink) == ConfigStatus::Ok);
  REQUIRE(engine.add_state_rule(StateRuleConfig{
              SignalCondition{"body.door_open", Comparison::Equal, RuleOperand::boolean(true)},
              ActionId{kCourtesyLight}}) == ConfigStatus::Ok);
  REQUIRE(engine.add_range_rule(tachometer()) == ConfigStatus::Ok);
  REQUIRE(engine.attach() == SignalStatus::Ok);
  provider.fake.start();

  // The provider's serial dispatcher is this notifying thread; the main
  // thread samples. Every notice and every sample changes a level.
  std::thread dispatcher{[&provider] {
    for (int round = 0; round < kRounds; ++round) {
      (void)provider.fake.publish(door(round % 2 == 0));
    }
  }};
  for (int round = 0; round < kRounds; ++round) {
    CHECK(engine.sample_polled_rules() == SignalStatus::Ok);
  }
  dispatcher.join();

  CHECK_FALSE(sink.overlapped());
  CHECK(sink.executed() == 2U * kRounds);
  provider.fake.stop();
  CHECK(engine.detach() == SignalStatus::Ok);
}
