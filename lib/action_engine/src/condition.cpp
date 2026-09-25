#include "action_engine/condition.hpp"

#include <cmath>

namespace action_engine {

using vehicle_signals::SignalCapability;
using vehicle_signals::SignalMetadata;
using vehicle_signals::SignalReading;
using vehicle_signals::SignalType;
using vehicle_signals::SignalValue;

namespace {

[[nodiscard]] bool is_ordered(Comparison comparison) noexcept {
  return comparison != Comparison::Equal && comparison != Comparison::NotEqual;
}

[[nodiscard]] SignalType operand_type(RuleOperand::Kind kind) noexcept {
  switch (kind) {
  case RuleOperand::Kind::Boolean:
    return SignalType::Boolean;
  case RuleOperand::Kind::Number:
    return SignalType::Number;
  case RuleOperand::Kind::Choice:
    return SignalType::Enum;
  }
  return SignalType::Boolean;
}

[[nodiscard]] ConditionResolution failure(ConfigStatus status) noexcept {
  return ConditionResolution{status, std::nullopt};
}

// Converts an operand already checked against the signal type.
[[nodiscard]] ConditionResolution resolve_operand(const SignalMetadata &signal,
                                                  const SignalCondition &condition,
                                                  FreshnessRequirement freshness) noexcept {
  const RuleOperand &operand = condition.operand;
  SignalValue value = SignalValue::boolean(operand.as_boolean().value_or(false));
  if (const auto number = operand.as_number()) {
    value = SignalValue::number(*number);
  }
  if (const auto key = operand.choice_key()) {
    const auto *choice = signal.find_choice(*key);
    if (choice == nullptr) {
      return failure(ConfigStatus::UnknownChoice);
    }
    value = SignalValue::enumeration(choice->value);
  }
  return ConditionResolution{ConfigStatus::Ok,
                             ResolvedCondition{signal.id, condition.comparison, value, freshness}};
}

// Shared resolution; `delivery` is the capability the rule consumes the
// signal through (Notify for notified rules, Read for sampled ones).
[[nodiscard]] ConditionResolution resolve_through(vehicle_signals::SignalCatalogView catalog,
                                                  const SignalCondition &condition,
                                                  FreshnessRequirement freshness,
                                                  SignalCapability delivery) noexcept {
  const SignalMetadata *signal = catalog.find(condition.signal_key);
  if (signal == nullptr) {
    return failure(ConfigStatus::UnknownSignal);
  }
  if (!signal->capabilities.has(delivery)) {
    return failure(ConfigStatus::UnsupportedCapability);
  }
  if (operand_type(condition.operand.kind()) != signal->type) {
    return failure(ConfigStatus::TypeMismatch);
  }
  if (!std::isfinite(condition.operand.as_number().value_or(0.0F))) {
    return failure(ConfigStatus::InvalidOperand);
  }
  if (is_ordered(condition.comparison) && signal->type != SignalType::Number) {
    return failure(ConfigStatus::UnsupportedComparison);
  }
  return resolve_operand(*signal, condition, freshness);
}

} // namespace

std::optional<bool> ResolvedCondition::evaluate(const SignalReading &reading) const noexcept {
  const auto value = actionable_value(reading, operand_.type(), freshness_);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return holds(*value);
}

bool ResolvedCondition::holds(const SignalValue &value) const noexcept {
  // Ordered comparisons are resolved only for Number signals.
  const float left = value.as_number().value_or(0.0F);
  const float right = operand_.as_number().value_or(0.0F);
  switch (comparison_) {
  case Comparison::Equal:
    return value == operand_;
  case Comparison::NotEqual:
    return value != operand_;
  case Comparison::Less:
    return left < right;
  case Comparison::LessOrEqual:
    return left <= right;
  case Comparison::Greater:
    return left > right;
  case Comparison::GreaterOrEqual:
    return left >= right;
  }
  return false;
}

ConditionResolution resolve_condition(vehicle_signals::SignalCatalogView catalog,
                                      const SignalCondition &condition,
                                      FreshnessRequirement freshness) noexcept {
  return resolve_through(catalog, condition, freshness, SignalCapability::Notify);
}

ConditionResolution resolve_sampled_condition(vehicle_signals::SignalCatalogView catalog,
                                              const SignalCondition &condition,
                                              FreshnessRequirement freshness) noexcept {
  return resolve_through(catalog, condition, freshness, SignalCapability::Read);
}

} // namespace action_engine
