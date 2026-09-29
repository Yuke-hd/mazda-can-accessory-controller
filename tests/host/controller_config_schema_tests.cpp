#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "controller_config/schema.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

#include <limits>
#include <string>
#include <variant>

namespace {

using controller_config::Configuration;
using controller_config::RuleSpec;
using controller_config::SchemaError;
using controller_config::ValidationResult;

constexpr vehicle_signals::SignalEnumChoice kTurnChoices[] = {
    {1, "left"}, {2, "right"}, {3, "hazard"}};
constexpr vehicle_signals::SignalMetadata kSignals[] = {
    {vehicle_signals::SignalId{1}, "vehicle.turn_state", vehicle_signals::SignalType::Enum,
     vehicle_signals::SignalUnit::None, vehicle_signals::ValidationStatus::Reference,
     vehicle_signals::SignalCapability::Notify, kTurnChoices, 3},
    {vehicle_signals::SignalId{2}, "vehicle.engine_rpm", vehicle_signals::SignalType::Number,
     vehicle_signals::SignalUnit::RevolutionsPerMinute,
     vehicle_signals::ValidationStatus::Reference, vehicle_signals::SignalCapability::Read, nullptr,
     0}};
constexpr vehicle_signals::SignalCatalogView kCatalog{kSignals};
static_assert(kCatalog.well_formed());

Configuration production() { return controller_config::default_configuration(); }

TEST_CASE("the persisted production configuration is a complete version-one profile") {
  const Configuration config = production();

  CHECK(config.version == 1U);
  CHECK(config.actions.size() == 5U);
  CHECK(config.rules.size() == 5U);
  CHECK(config.effect_bindings.size() == 5U);
  CHECK(config.fill_bindings.size() == 1U);
  CHECK(config.actions[0].name == "left_turn");
  CHECK(config.actions[1].name == "right_turn");
  CHECK(config.actions[2].name == "hazard");
  CHECK(config.action_id("left_turn") == action_engine::ActionId{1});
  CHECK(config.action_id("red_zone") == action_engine::ActionId{5});
  CHECK(config.rules[0].type == "state");
  CHECK(config.rules[0].signal == "vehicle.turn_state");
  CHECK(config.rules[0].comparison == "equal");
  CHECK(config.rules[0].operand.choice_value == "left");
  CHECK(config.rules[1].operand.choice_value == "right");
  CHECK(config.rules[2].operand.choice_value == "hazard");
  CHECK(config.rules[3].type == "range");
  CHECK(config.rules[3].input.from == 0.0F);
  CHECK(config.rules[3].input.to == 6500.0F);
  CHECK(config.rules[3].output.from == 0.0F);
  CHECK(config.rules[3].output.to == 1.0F);
  CHECK(config.rules[3].freshness == "fresh_or_unverified");
  CHECK(config.rules[4].type == "sampled_state");
  CHECK(config.rules[4].comparison == "greater");
  CHECK(config.rules[4].operand.number_value == 6000.0F);
  CHECK(config.rules[4].freshness == "fresh_or_unverified");
  CHECK(config.effect_bindings[0].action == "left_turn");
  CHECK(config.effect_bindings[0].effect == "right_turn");
  CHECK(config.effect_bindings[2].action == "hazard");
  CHECK(config.effect_bindings[2].effect == "left_turn");
  CHECK(config.effect_bindings[3].effect == "right_turn");
  CHECK(config.fill_bindings[0].zone.start == 0U);
  CHECK(config.fill_bindings[0].zone.length == 100U);
  CHECK(config.fill_bindings[0].zone.direction == "center_out");
  CHECK(config.fill_bindings[0].color.red == 0);
  CHECK(config.fill_bindings[0].color.green == 16);
  CHECK(config.fill_bindings[0].color.blue == 32);
  CHECK(config.fill_bindings[0].priority == 50);
  CHECK(config.effect_bindings[4].effect == "brake");
  CHECK(config.effect_bindings[4].priority == 150);
  CHECK(controller_config::validate(config).ok());
}

TEST_CASE("actions must be named uniquely and every rule and binding references one") {
  Configuration config = production();
  config.actions[1].name = config.actions[0].name;
  CHECK(controller_config::validate(config).error == SchemaError::DuplicateActionName);

  config = production();
  config.rules[0].action = "does-not-exist";
  CHECK(controller_config::validate(config).error == SchemaError::UnknownAction);

  config = production();
  config.effect_bindings[0].action.clear();
  CHECK(controller_config::validate(config).error == SchemaError::UnknownAction);
}

TEST_CASE("rule and binding enum names are validated before runtime conversion") {
  Configuration config = production();
  config.rules[0].type = "not-a-rule";
  CHECK(controller_config::validate(config).error == SchemaError::UnknownRuleType);

  config = production();
  config.rules[0].comparison = "not-a-comparison";
  CHECK(controller_config::validate(config).error == SchemaError::UnknownComparison);

  config = production();
  config.rules[0].operand = controller_config::OperandSpec::choice("left");
  config.rules[0].comparison = "greater";
  CHECK(controller_config::validate(config).error == SchemaError::OperandComparisonMismatch);

  config = production();
  config.effect_bindings[0].effect = "not-an-effect";
  CHECK(controller_config::validate(config).error == SchemaError::UnknownLedEffect);

  config = production();
  config.fill_bindings[0].zone.direction = "not-a-direction";
  CHECK(controller_config::validate(config).error == SchemaError::UnknownFillDirection);
}

TEST_CASE("numbers, ranges, zones, colors and priorities stay within their persisted bounds") {
  Configuration config = production();
  config.rules[3].input.from = std::numeric_limits<float>::quiet_NaN();
  CHECK(controller_config::validate(config).error == SchemaError::NonFiniteNumber);

  config = production();
  config.rules[3].input.to = config.rules[3].input.from;
  CHECK(controller_config::validate(config).error == SchemaError::InvalidRange);

  config = production();
  config.fill_bindings[0].zone.length = 101U;
  CHECK(controller_config::validate(config).error == SchemaError::InvalidLedZone);

  config = production();
  config.fill_bindings[0].priority = 256;
  CHECK(controller_config::validate(config).error == SchemaError::InvalidPriority);

  config = production();
  config.fill_bindings[0].color.red = 256;
  CHECK(controller_config::validate(config).error == SchemaError::InvalidRgb);
}

TEST_CASE("owned persisted strings remain valid after source temporaries are gone") {
  Configuration config = production();
  const std::string signal = config.rules[0].signal;
  const std::string action = config.rules[0].action;
  config.rules[0].signal = signal;
  config.rules[0].action = action;

  CHECK(controller_config::validate(config).ok());
  CHECK(config.rules[0].signal == signal);
  CHECK(config.rules[0].action == action);
}

TEST_CASE("production rules convert to the generic action-engine persisted concepts") {
  const Configuration config = production();
  REQUIRE(controller_config::validate(config, kCatalog).ok());

  const auto turn = controller_config::to_action_engine_rule(config, 0);
  REQUIRE(turn.has_value());
  const auto &state = std::get<action_engine::StateRuleConfig>(*turn);
  CHECK(state.action == action_engine::ActionId{1});
  CHECK(state.condition.signal_key == "vehicle.turn_state");
  CHECK(state.condition.comparison == action_engine::Comparison::Equal);
  CHECK(state.condition.operand.choice_key() == "left");

  const auto range = controller_config::to_action_engine_rule(config, 3);
  REQUIRE(range.has_value());
  const auto &rpm = std::get<action_engine::RangeRuleConfig>(*range);
  CHECK(rpm.input.from == 0.0F);
  CHECK(rpm.input.to == 6500.0F);
  CHECK(rpm.output.from == 0.0F);
  CHECK(rpm.output.to == 1.0F);
  CHECK(rpm.freshness == action_engine::FreshnessRequirement::FreshOrUnverified);

  const auto threshold = controller_config::to_action_engine_rule(config, 4);
  REQUIRE(threshold.has_value());
  const auto &red_zone = std::get<action_engine::SampledStateRuleConfig>(*threshold);
  CHECK(red_zone.condition.comparison == action_engine::Comparison::Greater);
  CHECK(red_zone.condition.operand.as_number() == 6000.0F);
  CHECK(red_zone.freshness == action_engine::FreshnessRequirement::FreshOrUnverified);
}

TEST_CASE("catalog validation delegates signal type and capability checks to the action engine") {
  Configuration config = production();
  config.rules[0].operand = controller_config::OperandSpec::boolean(true);
  CHECK(controller_config::validate(config, kCatalog).error == SchemaError::TypeMismatch);

  config = production();
  config.rules[3].signal = "vehicle.turn_state";
  CHECK(controller_config::validate(config, kCatalog).error == SchemaError::UnsupportedCapability);

  config = production();
  config.rules[0].operand = controller_config::OperandSpec::choice("missing");
  CHECK(controller_config::validate(config, kCatalog).error == SchemaError::UnknownChoice);
}

TEST_CASE("versioning and event rules stay parser-independent") {
  Configuration config = production();
  config.version = 2U;
  CHECK(controller_config::validate(config).error == SchemaError::UnsupportedVersion);

  config = production();
  RuleSpec event{};
  event.type = "event";
  event.action = "red_zone";
  event.signal = "vehicle.turn_state";
  event.comparison = "equal";
  event.operand = controller_config::OperandSpec::choice("hazard");
  event.freshness = "fresh";
  event.edge = "becomes_false";
  config.rules.push_back(event);

  REQUIRE(controller_config::validate(config, kCatalog).ok());
  const auto converted = controller_config::to_action_engine_rule(config, config.rules.size() - 1U);
  REQUIRE(converted.has_value());
  const auto &event_rule = std::get<action_engine::EventRuleConfig>(*converted);
  CHECK(event_rule.edge == action_engine::EventEdge::BecomesFalse);
  CHECK(event_rule.action == action_engine::ActionId{5});
}

} // namespace
