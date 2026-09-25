#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "action_engine_fixture.hpp"

#include <limits>
#include <optional>
#include <string_view>

#include "action_engine/condition.hpp"
#include "action_engine/rule_config.hpp"

namespace {

using namespace action_engine_fixture;
using action_engine::Comparison;
using action_engine::ConditionResolution;
using action_engine::ConfigStatus;
using action_engine::FreshnessRequirement;
using action_engine::ResolvedCondition;
using action_engine::RuleOperand;
using action_engine::SignalCondition;
using vehicle_signals::SignalReading;

ConditionResolution resolve(std::string_view key, Comparison comparison, RuleOperand operand,
                            FreshnessRequirement freshness = FreshnessRequirement::Fresh) {
  return action_engine::resolve_condition(kView, SignalCondition{key, comparison, operand},
                                          freshness);
}

ConfigStatus status_of(std::string_view key, Comparison comparison, RuleOperand operand) {
  return resolve(key, comparison, operand).status;
}

SignalReading reading(SignalValue value, Availability availability = Availability::Fresh) {
  return SignalReading{value, availability, ValidationStatus::Reference};
}

// Condition truth for a Fresh reading; nullopt means "not actionable".
std::optional<bool> truth(const ConditionResolution &resolution, SignalValue value,
                          Availability availability = Availability::Fresh) {
  REQUIRE(resolution.condition.has_value());
  return resolution.condition->evaluate(reading(value, availability));
}

} // namespace

TEST_CASE("rule operands are tagged boolean, number, or enum choice key values") {
  const RuleOperand open = RuleOperand::boolean(true);
  CHECK(open.as_boolean() == true);
  CHECK_FALSE(open.as_number().has_value());
  CHECK_FALSE(open.choice_key().has_value());

  const RuleOperand limit = RuleOperand::number(30.5F);
  CHECK(limit.as_number() == 30.5F);
  CHECK_FALSE(limit.as_boolean().has_value());

  const RuleOperand reverse = RuleOperand::choice("reverse");
  CHECK(reverse.choice_key() == std::optional<std::string_view>{"reverse"});
  CHECK_FALSE(reverse.as_number().has_value());

  CHECK(RuleOperand{}.as_boolean() == false);
}

TEST_CASE("body.door_open == true resolves to the catalog id with a boolean operand") {
  const auto resolution = resolve("body.door_open", Comparison::Equal, RuleOperand::boolean(true));
  CHECK(resolution.status == ConfigStatus::Ok);
  REQUIRE(resolution.condition.has_value());
  CHECK(resolution.condition->signal() == kDoorOpen);
  CHECK(truth(resolution, SignalValue::boolean(true)) == true);
  CHECK(truth(resolution, SignalValue::boolean(false)) == false);
}

TEST_CASE("transmission.gear == reverse resolves the choice key to raw value 1") {
  const auto resolution =
      resolve("transmission.gear", Comparison::Equal, RuleOperand::choice("reverse"));
  CHECK(resolution.status == ConfigStatus::Ok);
  REQUIRE(resolution.condition.has_value());
  CHECK(resolution.condition->signal() == kGear);
  CHECK(truth(resolution, SignalValue::enumeration(kReverse)) == true);
  CHECK(truth(resolution, SignalValue::enumeration(kDrive)) == false);

  const auto not_park =
      resolve("transmission.gear", Comparison::NotEqual, RuleOperand::choice("park"));
  CHECK(truth(not_park, SignalValue::enumeration(kPark)) == false);
  CHECK(truth(not_park, SignalValue::enumeration(kNeutral)) == true);
}

TEST_CASE("motion.speed_kph comparisons against 30 cover every ordered operator") {
  const RuleOperand thirty = RuleOperand::number(30.0F);
  const auto greater = resolve("motion.speed_kph", Comparison::Greater, thirty);
  const auto at_least = resolve("motion.speed_kph", Comparison::GreaterOrEqual, thirty);
  const auto less = resolve("motion.speed_kph", Comparison::Less, thirty);
  const auto at_most = resolve("motion.speed_kph", Comparison::LessOrEqual, thirty);
  const auto equal = resolve("motion.speed_kph", Comparison::Equal, thirty);
  const auto not_equal = resolve("motion.speed_kph", Comparison::NotEqual, thirty);

  CHECK(truth(greater, SignalValue::number(30.0F)) == false);
  CHECK(truth(greater, SignalValue::number(30.5F)) == true);
  CHECK(truth(at_least, SignalValue::number(30.0F)) == true);
  CHECK(truth(at_least, SignalValue::number(29.5F)) == false);
  CHECK(truth(less, SignalValue::number(29.5F)) == true);
  CHECK(truth(less, SignalValue::number(30.0F)) == false);
  CHECK(truth(at_most, SignalValue::number(30.0F)) == true);
  CHECK(truth(at_most, SignalValue::number(30.5F)) == false);
  CHECK(truth(equal, SignalValue::number(30.0F)) == true);
  CHECK(truth(equal, SignalValue::number(31.0F)) == false);
  CHECK(truth(not_equal, SignalValue::number(31.0F)) == true);
  CHECK(truth(not_equal, SignalValue::number(30.0F)) == false);
}

TEST_CASE("an unknown key body.sunroof_open is rejected as UnknownSignal") {
  CHECK(status_of("body.sunroof_open", Comparison::Equal, RuleOperand::boolean(true)) ==
        ConfigStatus::UnknownSignal);
  CHECK(status_of("", Comparison::Equal, RuleOperand::boolean(true)) ==
        ConfigStatus::UnknownSignal);
  CHECK_FALSE(resolve("body.sunroof_open", Comparison::Equal, RuleOperand::boolean(true))
                  .condition.has_value());
}

TEST_CASE("the read-only trip.odometer_km is rejected as UnsupportedCapability") {
  CHECK(status_of("trip.odometer_km", Comparison::Greater, RuleOperand::number(1.0F)) ==
        ConfigStatus::UnsupportedCapability);
}

TEST_CASE("operands whose type differs from the signal type are TypeMismatch") {
  CHECK(status_of("motion.speed_kph", Comparison::Equal, RuleOperand::boolean(true)) ==
        ConfigStatus::TypeMismatch);
  CHECK(status_of("body.door_open", Comparison::Equal, RuleOperand::number(1.0F)) ==
        ConfigStatus::TypeMismatch);
  CHECK(status_of("motion.speed_kph", Comparison::Equal, RuleOperand::choice("drive")) ==
        ConfigStatus::TypeMismatch);
  CHECK(status_of("transmission.gear", Comparison::Equal, RuleOperand::number(3.0F)) ==
        ConfigStatus::TypeMismatch);
  CHECK(status_of("body.door_open", Comparison::Equal, RuleOperand::choice("open")) ==
        ConfigStatus::TypeMismatch);
}

TEST_CASE("choice key sport is not a transmission.gear choice and is UnknownChoice") {
  CHECK(status_of("transmission.gear", Comparison::Equal, RuleOperand::choice("sport")) ==
        ConfigStatus::UnknownChoice);
  CHECK(status_of("transmission.gear", Comparison::Equal, RuleOperand::choice("")) ==
        ConfigStatus::UnknownChoice);
}

TEST_CASE("non-finite speed thresholds are InvalidOperand") {
  CHECK(status_of("motion.speed_kph", Comparison::Greater,
                  RuleOperand::number(std::numeric_limits<float>::quiet_NaN())) ==
        ConfigStatus::InvalidOperand);
  CHECK(status_of("motion.speed_kph", Comparison::Less,
                  RuleOperand::number(std::numeric_limits<float>::infinity())) ==
        ConfigStatus::InvalidOperand);
  CHECK(status_of("motion.speed_kph", Comparison::Less,
                  RuleOperand::number(-std::numeric_limits<float>::infinity())) ==
        ConfigStatus::InvalidOperand);
}

TEST_CASE("ordered comparisons on boolean and enum signals are UnsupportedComparison") {
  CHECK(status_of("body.door_open", Comparison::Less, RuleOperand::boolean(true)) ==
        ConfigStatus::UnsupportedComparison);
  CHECK(status_of("transmission.gear", Comparison::GreaterOrEqual, RuleOperand::choice("drive")) ==
        ConfigStatus::UnsupportedComparison);
  CHECK(status_of("body.door_open", Comparison::NotEqual, RuleOperand::boolean(true)) ==
        ConfigStatus::Ok);
}

TEST_CASE("NoData, Stale and Unavailable readings are never actionable") {
  const auto open = resolve("body.door_open", Comparison::Equal, RuleOperand::boolean(true),
                            FreshnessRequirement::FreshOrUnverified);
  REQUIRE(open.condition.has_value());
  const ResolvedCondition &condition = *open.condition;

  CHECK_FALSE(condition.evaluate(SignalReading{}).has_value());
  CHECK_FALSE(
      condition
          .evaluate(SignalReading{std::nullopt, Availability::Fresh, ValidationStatus::Reference})
          .has_value());
  CHECK_FALSE(
      condition.evaluate(reading(SignalValue::boolean(true), Availability::NoData)).has_value());
  CHECK_FALSE(
      condition.evaluate(reading(SignalValue::boolean(true), Availability::Stale)).has_value());
  CHECK_FALSE(condition.evaluate(reading(SignalValue::boolean(true), Availability::Unavailable))
                  .has_value());
}

TEST_CASE("FreshnessUnverified is actionable only under the FreshOrUnverified policy") {
  const auto strict = resolve("body.door_open", Comparison::Equal, RuleOperand::boolean(true));
  const auto lenient = resolve("body.door_open", Comparison::Equal, RuleOperand::boolean(true),
                               FreshnessRequirement::FreshOrUnverified);
  const auto unverified_open =
      reading(SignalValue::boolean(true), Availability::FreshnessUnverified);

  CHECK_FALSE(strict.condition->evaluate(unverified_open).has_value());
  CHECK(lenient.condition->evaluate(unverified_open) == true);
  CHECK(strict.condition->evaluate(reading(SignalValue::boolean(true))) == true);
}

TEST_CASE("mistyped and non-finite readings are not actionable") {
  const auto fast = resolve("motion.speed_kph", Comparison::Greater, RuleOperand::number(30.0F));
  REQUIRE(fast.condition.has_value());
  CHECK_FALSE(fast.condition->evaluate(reading(SignalValue::boolean(true))).has_value());
  CHECK_FALSE(fast.condition->evaluate(reading(SignalValue::enumeration(40))).has_value());
  CHECK_FALSE(fast.condition
                  ->evaluate(reading(SignalValue::number(std::numeric_limits<float>::quiet_NaN())))
                  .has_value());
  CHECK_FALSE(
      fast.condition->evaluate(reading(SignalValue::number(std::numeric_limits<float>::infinity())))
          .has_value());
}

TEST_CASE("freshness policies map availability states explicitly") {
  using action_engine::meets;
  CHECK(meets(FreshnessRequirement::Fresh, Availability::Fresh));
  CHECK_FALSE(meets(FreshnessRequirement::Fresh, Availability::FreshnessUnverified));
  CHECK(meets(FreshnessRequirement::FreshOrUnverified, Availability::FreshnessUnverified));
  CHECK(meets(FreshnessRequirement::FreshOrUnverified, Availability::Fresh));
  for (const auto requirement :
       {FreshnessRequirement::Fresh, FreshnessRequirement::FreshOrUnverified}) {
    CHECK_FALSE(meets(requirement, Availability::NoData));
    CHECK_FALSE(meets(requirement, Availability::Stale));
    CHECK_FALSE(meets(requirement, Availability::Unavailable));
  }
}
