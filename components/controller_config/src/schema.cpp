#include "controller_config/schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

namespace controller_config {
namespace {

using action_engine::Comparison;
using action_engine::ConfigStatus;
using action_engine::EventEdge;
using action_engine::FreshnessRequirement;
using action_engine::RuleOperand;
using action_engine::SignalCondition;
using local_argb::internal::FillDirection;
using local_argb_actions::LedEffect;

[[nodiscard]] bool is_blank(const std::string_view value) noexcept {
  if (value.empty())
    return true;
  return std::all_of(value.begin(), value.end(), [](const unsigned char character) noexcept {
    return std::isspace(character) != 0;
  });
}

[[nodiscard]] bool named(const std::string_view value, const std::string_view canonical,
                         const std::string_view pascal) noexcept {
  return value == canonical || value == pascal;
}

[[nodiscard]] bool ordered(const Comparison comparison) noexcept {
  return comparison != Comparison::Equal && comparison != Comparison::NotEqual;
}

[[nodiscard]] ValidationResult failure(const SchemaError error,
                                       const std::size_t index = 0) noexcept {
  return ValidationResult{error, index};
}

[[nodiscard]] std::optional<std::size_t> action_index(const Configuration &configuration,
                                                      const std::string_view name) noexcept {
  for (std::size_t index = 0; index < configuration.actions.size(); ++index) {
    if (configuration.actions[index].name == name)
      return index;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RuleOperand> to_rule_operand(const OperandSpec &operand) noexcept {
  switch (operand.kind) {
  case OperandKind::Boolean:
    return RuleOperand::boolean(operand.boolean_value);
  case OperandKind::Number:
    return RuleOperand::number(operand.number_value);
  case OperandKind::Choice:
    return RuleOperand::choice(operand.choice_value);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<SignalCondition> to_condition(const RuleSpec &rule) noexcept {
  const auto comparison = comparison_from_name(rule.comparison);
  const auto operand = to_rule_operand(rule.operand);
  const auto freshness = freshness_from_name(rule.freshness);
  if (!comparison.has_value() || !operand.has_value() || !freshness.has_value())
    return std::nullopt;
  return SignalCondition{rule.signal, *comparison, *operand};
}

[[nodiscard]] SchemaError status_error(const ConfigStatus status) noexcept {
  switch (status) {
  case ConfigStatus::Ok:
    return SchemaError::None;
  case ConfigStatus::UnknownSignal:
    return SchemaError::UnknownSignal;
  case ConfigStatus::UnsupportedCapability:
    return SchemaError::UnsupportedCapability;
  case ConfigStatus::TypeMismatch:
    return SchemaError::TypeMismatch;
  case ConfigStatus::UnknownChoice:
    return SchemaError::UnknownChoice;
  case ConfigStatus::InvalidOperand:
    return SchemaError::NonFiniteNumber;
  case ConfigStatus::UnsupportedComparison:
    return SchemaError::UnsupportedComparison;
  case ConfigStatus::InvalidRange:
    return SchemaError::InvalidRange;
  case ConfigStatus::InvalidHysteresis:
    return SchemaError::InvalidHysteresis;
  case ConfigStatus::InvalidState:
  case ConfigStatus::CapacityExceeded:
  case ConfigStatus::DuplicateSink:
  case ConfigStatus::InvalidAction:
  case ConfigStatus::DuplicateAction:
    return SchemaError::UnknownAction;
  }
  return SchemaError::UnknownRuleType;
}

[[nodiscard]] ValidationResult validate_rule_shape(const RuleSpec &rule) noexcept {
  const auto type = rule.type;
  const bool is_state = named(type, "state", "State");
  const bool is_sampled = named(type, "sampled_state", "SampledState");
  const bool is_event = named(type, "event", "Event");
  const bool is_range = named(type, "range", "Range");
  if (!is_state && !is_sampled && !is_event && !is_range)
    return failure(SchemaError::UnknownRuleType);
  if (is_blank(rule.signal))
    return failure(SchemaError::EmptySignal);
  if (!freshness_from_name(rule.freshness).has_value())
    return failure(SchemaError::UnknownFreshness);

  if (is_range) {
    const bool finite = std::isfinite(rule.input.from) && std::isfinite(rule.input.to) &&
                        std::isfinite(rule.input.to - rule.input.from) &&
                        std::isfinite(rule.output.from) && std::isfinite(rule.output.to) &&
                        std::isfinite(rule.output.to - rule.output.from);
    if (!finite)
      return failure(SchemaError::NonFiniteNumber);
    if (!(rule.input.from < rule.input.to))
      return failure(SchemaError::InvalidRange);
    return ValidationResult{};
  }

  const auto comparison = comparison_from_name(rule.comparison);
  if (!comparison.has_value())
    return failure(SchemaError::UnknownComparison);
  switch (rule.operand.kind) {
  case OperandKind::Boolean:
    break;
  case OperandKind::Number:
    if (!std::isfinite(rule.operand.number_value))
      return failure(SchemaError::NonFiniteNumber);
    break;
  case OperandKind::Choice:
    if (is_blank(rule.operand.choice_value))
      return failure(SchemaError::EmptyChoice);
    break;
  default:
    return failure(SchemaError::UnknownOperandKind);
  }
  if (rule.operand.kind != OperandKind::Number && ordered(*comparison))
    return failure(SchemaError::OperandComparisonMismatch);

  if (is_event && !event_edge_from_name(rule.edge).has_value())
    return failure(SchemaError::UnknownEventEdge);
  if (is_sampled && rule.release_threshold.has_value()) {
    if (!std::isfinite(*rule.release_threshold))
      return failure(SchemaError::NonFiniteNumber);
    if (rule.operand.kind != OperandKind::Number || !ordered(*comparison))
      return failure(SchemaError::InvalidHysteresis);
    const float activation = rule.operand.number_value;
    const float release = *rule.release_threshold;
    if (((*comparison == Comparison::Greater || *comparison == Comparison::GreaterOrEqual) &&
         !(release < activation)) ||
        ((*comparison == Comparison::Less || *comparison == Comparison::LessOrEqual) &&
         !(release > activation)))
      return failure(SchemaError::InvalidHysteresis);
  }
  return ValidationResult{};
}

[[nodiscard]] ValidationResult validate_actions(const Configuration &configuration) noexcept {
  for (std::size_t index = 0; index < configuration.actions.size(); ++index) {
    const ActionSpec &action = configuration.actions[index];
    if (is_blank(action.name))
      return failure(SchemaError::EmptyActionName, index);
    const std::uint32_t id = static_cast<std::uint32_t>(index + 1U);
    if (id > std::numeric_limits<std::uint16_t>::max())
      return failure(SchemaError::ActionIdExhausted, index);
    for (std::size_t other = 0; other < index; ++other) {
      if (configuration.actions[other].name == action.name)
        return failure(SchemaError::DuplicateActionName, index);
    }
  }
  return ValidationResult{};
}

[[nodiscard]] ValidationResult validate_bindings(const Configuration &configuration) noexcept {
  for (std::size_t index = 0; index < configuration.effect_bindings.size(); ++index) {
    const auto &binding = configuration.effect_bindings[index];
    if (!action_index(configuration, binding.action).has_value())
      return failure(SchemaError::UnknownAction, index);
    if (!led_effect_from_name(binding.effect).has_value())
      return failure(SchemaError::UnknownLedEffect, index);
    if (binding.priority < 0 || binding.priority > std::numeric_limits<std::uint8_t>::max())
      return failure(SchemaError::InvalidPriority, index);
  }
  for (std::size_t index = 0; index < configuration.fill_bindings.size(); ++index) {
    const auto &binding = configuration.fill_bindings[index];
    if (!action_index(configuration, binding.action).has_value())
      return failure(SchemaError::UnknownAction, index);
    if (!fill_direction_from_name(binding.zone.direction).has_value())
      return failure(SchemaError::UnknownFillDirection, index);
    if (binding.zone.start >= kLogicalStripPixelCount || binding.zone.length == 0U ||
        binding.zone.length > kLogicalStripPixelCount - binding.zone.start)
      return failure(SchemaError::InvalidLedZone, index);
    if (binding.color.red < 0 || binding.color.red > 255 || binding.color.green < 0 ||
        binding.color.green > 255 || binding.color.blue < 0 || binding.color.blue > 255)
      return failure(SchemaError::InvalidRgb, index);
    if (binding.priority < 0 || binding.priority > std::numeric_limits<std::uint8_t>::max())
      return failure(SchemaError::InvalidPriority, index);
  }
  return ValidationResult{};
}

[[nodiscard]] ValidationResult validate_shape(const Configuration &configuration) noexcept {
  if (configuration.version != kControllerConfigVersion)
    return failure(SchemaError::UnsupportedVersion);
  const auto actions = validate_actions(configuration);
  if (!actions.ok())
    return actions;
  for (std::size_t index = 0; index < configuration.rules.size(); ++index) {
    const auto &rule = configuration.rules[index];
    if (!action_index(configuration, rule.action).has_value())
      return failure(SchemaError::UnknownAction, index);
    const auto shape = validate_rule_shape(rule);
    if (!shape.ok()) {
      return ValidationResult{shape.error, index};
    }
  }
  return validate_bindings(configuration);
}

} // namespace

std::optional<Comparison> comparison_from_name(const std::string_view name) noexcept {
  if (named(name, "equal", "Equal"))
    return Comparison::Equal;
  if (named(name, "not_equal", "NotEqual"))
    return Comparison::NotEqual;
  if (named(name, "less", "Less"))
    return Comparison::Less;
  if (named(name, "less_or_equal", "LessOrEqual"))
    return Comparison::LessOrEqual;
  if (named(name, "greater", "Greater"))
    return Comparison::Greater;
  if (named(name, "greater_or_equal", "GreaterOrEqual"))
    return Comparison::GreaterOrEqual;
  return std::nullopt;
}

std::optional<FreshnessRequirement> freshness_from_name(const std::string_view name) noexcept {
  if (named(name, "fresh", "Fresh"))
    return FreshnessRequirement::Fresh;
  if (named(name, "fresh_or_unverified", "FreshOrUnverified"))
    return FreshnessRequirement::FreshOrUnverified;
  return std::nullopt;
}

std::optional<EventEdge> event_edge_from_name(const std::string_view name) noexcept {
  if (named(name, "becomes_true", "BecomesTrue"))
    return EventEdge::BecomesTrue;
  if (named(name, "becomes_false", "BecomesFalse"))
    return EventEdge::BecomesFalse;
  return std::nullopt;
}

std::optional<OperandKind> operand_kind_from_name(const std::string_view name) noexcept {
  if (named(name, "boolean", "Boolean"))
    return OperandKind::Boolean;
  if (named(name, "number", "Number"))
    return OperandKind::Number;
  if (named(name, "choice", "Choice"))
    return OperandKind::Choice;
  return std::nullopt;
}

std::optional<LedEffect> led_effect_from_name(const std::string_view name) noexcept {
  if (named(name, "left_turn", "LeftTurn"))
    return LedEffect::LeftTurn;
  if (named(name, "right_turn", "RightTurn"))
    return LedEffect::RightTurn;
  if (named(name, "brake", "Brake"))
    return LedEffect::Brake;
  return std::nullopt;
}

std::optional<FillDirection> fill_direction_from_name(const std::string_view name) noexcept {
  if (named(name, "start_to_end", "StartToEnd"))
    return FillDirection::StartToEnd;
  if (named(name, "end_to_start", "EndToStart"))
    return FillDirection::EndToStart;
  if (named(name, "center_out", "CenterOut"))
    return FillDirection::CenterOut;
  return std::nullopt;
}

std::optional<action_engine::ActionId>
Configuration::action_id(const std::string_view name) const noexcept {
  const auto index = action_index(*this, name);
  if (!index.has_value())
    return std::nullopt;
  const std::uint32_t value = static_cast<std::uint32_t>(*index + 1U);
  if (value == 0U || value > std::numeric_limits<std::uint16_t>::max())
    return std::nullopt;
  return action_engine::ActionId{static_cast<std::uint16_t>(value)};
}

std::optional<EngineRule> to_action_engine_rule(const Configuration &configuration,
                                                const std::size_t rule_index) noexcept {
  if (rule_index >= configuration.rules.size())
    return std::nullopt;
  const RuleSpec &rule = configuration.rules[rule_index];
  const auto action = configuration.action_id(rule.action);
  const auto freshness = freshness_from_name(rule.freshness);
  if (!action.has_value() || !freshness.has_value())
    return std::nullopt;
  const auto type = rule.type;
  if (named(type, "range", "Range")) {
    return EngineRule{
        action_engine::RangeRuleConfig{rule.signal, rule.input, rule.output, *action, *freshness}};
  }
  const auto condition = to_condition(rule);
  if (!condition.has_value())
    return std::nullopt;
  if (named(type, "state", "State"))
    return EngineRule{action_engine::StateRuleConfig{*condition, *action, *freshness}};
  if (named(type, "sampled_state", "SampledState")) {
    return EngineRule{action_engine::SampledStateRuleConfig{*condition, *action, *freshness,
                                                            rule.release_threshold}};
  }
  if (named(type, "event", "Event")) {
    const auto edge = event_edge_from_name(rule.edge);
    if (!edge.has_value())
      return std::nullopt;
    return EngineRule{action_engine::EventRuleConfig{*condition, *edge, *action, *freshness}};
  }
  return std::nullopt;
}

ValidationResult validate(const Configuration &configuration) noexcept {
  return validate_shape(configuration);
}

ValidationResult validate(const Configuration &configuration,
                          const vehicle_signals::SignalCatalogView catalog) noexcept {
  const auto shape = validate_shape(configuration);
  if (!shape.ok())
    return shape;
  for (std::size_t index = 0; index < configuration.rules.size(); ++index) {
    const auto runtime = to_action_engine_rule(configuration, index);
    if (!runtime.has_value())
      return failure(SchemaError::UnknownRuleType, index);
    SchemaError error = SchemaError::None;
    std::visit(
        [&catalog, &error](const auto &rule) noexcept {
          using Rule = std::decay_t<decltype(rule)>;
          if constexpr (std::is_same_v<Rule, action_engine::StateRuleConfig>) {
            error = status_error(
                action_engine::resolve_condition(catalog, rule.condition, rule.freshness).status);
          } else if constexpr (std::is_same_v<Rule, action_engine::EventRuleConfig>) {
            error = status_error(
                action_engine::resolve_condition(catalog, rule.condition, rule.freshness).status);
          } else if constexpr (std::is_same_v<Rule, action_engine::SampledStateRuleConfig>) {
            error = status_error(
                action_engine::resolve_sampled_condition(catalog, rule.condition, rule.freshness)
                    .status);
          } else {
            error = status_error(action_engine::resolve_range_rule(catalog, rule).status);
          }
        },
        *runtime);
    if (error != SchemaError::None)
      return failure(error, index);
  }
  return ValidationResult{};
}

Configuration default_configuration() {
  Configuration configuration{};
  configuration.actions = {{"left_turn"}, {"right_turn"}, {"hazard"}, {"rpm_fill"}, {"red_zone"}};

  const auto turn_rule = [](std::string action, std::string choice) {
    RuleSpec rule{};
    rule.type = "state";
    rule.action = std::move(action);
    rule.signal = "vehicle.turn_state";
    rule.comparison = "equal";
    rule.operand = OperandSpec::choice(std::move(choice));
    rule.freshness = "fresh";
    return rule;
  };
  configuration.rules.push_back(turn_rule("left_turn", "left"));
  configuration.rules.push_back(turn_rule("right_turn", "right"));
  configuration.rules.push_back(turn_rule("hazard", "hazard"));

  RuleSpec rpm_fill{};
  rpm_fill.type = "range";
  rpm_fill.action = "rpm_fill";
  rpm_fill.signal = "vehicle.engine_rpm";
  rpm_fill.freshness = "fresh_or_unverified";
  rpm_fill.input = action_engine::NumericRange{0.0F, 6500.0F};
  rpm_fill.output = action_engine::NumericRange{0.0F, 1.0F};
  configuration.rules.push_back(std::move(rpm_fill));

  RuleSpec red_zone{};
  red_zone.type = "sampled_state";
  red_zone.action = "red_zone";
  red_zone.signal = "vehicle.engine_rpm";
  red_zone.comparison = "greater";
  red_zone.operand = OperandSpec::number(6000.0F);
  red_zone.freshness = "fresh_or_unverified";
  configuration.rules.push_back(std::move(red_zone));

  configuration.effect_bindings = {{"left_turn", "right_turn", 100},
                                   {"right_turn", "left_turn", 100},
                                   {"hazard", "left_turn", 100},
                                   {"hazard", "right_turn", 100},
                                   {"red_zone", "brake", 150}};
  configuration.fill_bindings = {
      {"rpm_fill", ZoneSpec{0U, 100U, "center_out"}, RgbSpec{0, 16, 32}, 50}};
  return configuration;
}

} // namespace controller_config
