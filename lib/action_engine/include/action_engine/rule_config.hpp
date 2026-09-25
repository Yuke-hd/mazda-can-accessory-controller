#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "action_engine/action.hpp"

// Persisted-form rule configuration. Signals and enum choices are named by
// their catalog keys, never by per-build numeric ids or raw enum values; the
// engine resolves them through the provider catalog when a rule is added.
// String storage only needs to outlive that add_* call.

namespace action_engine {

// Right-hand side of a condition: a Boolean, a Number, or an Enum choice key
// such as "reverse". A default operand is Boolean false.
class RuleOperand final {
public:
  enum class Kind : std::uint8_t { Boolean, Number, Choice };

  constexpr RuleOperand() noexcept = default;

  [[nodiscard]] static constexpr RuleOperand boolean(bool value) noexcept {
    RuleOperand result{};
    result.boolean_ = value;
    return result;
  }
  [[nodiscard]] static constexpr RuleOperand number(float value) noexcept {
    RuleOperand result{};
    result.kind_ = Kind::Number;
    result.number_ = value;
    return result;
  }
  [[nodiscard]] static constexpr RuleOperand choice(std::string_view key) noexcept {
    RuleOperand result{};
    result.kind_ = Kind::Choice;
    result.choice_ = key;
    return result;
  }

  [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr std::optional<bool> as_boolean() const noexcept {
    return kind_ == Kind::Boolean ? std::optional<bool>{boolean_} : std::nullopt;
  }
  [[nodiscard]] constexpr std::optional<float> as_number() const noexcept {
    return kind_ == Kind::Number ? std::optional<float>{number_} : std::nullopt;
  }
  [[nodiscard]] constexpr std::optional<std::string_view> choice_key() const noexcept {
    return kind_ == Kind::Choice ? std::optional<std::string_view>{choice_} : std::nullopt;
  }

private:
  Kind kind_{Kind::Boolean};
  bool boolean_{false};
  float number_{0.0F};
  std::string_view choice_{};
};

// Equal and NotEqual apply to every signal type; the ordered comparisons apply
// only to Number signals.
enum class Comparison : std::uint8_t {
  Equal,
  NotEqual,
  Less,
  LessOrEqual,
  Greater,
  GreaterOrEqual
};

// Consumer-level availability policy. A reading is actionable only when it has
// a value of the signal's type (finite for Number) and its availability meets
// the requirement. NoData, Stale and Unavailable are never actionable.
enum class FreshnessRequirement : std::uint8_t {
  Fresh,             // Only Fresh readings (the strict default).
  FreshOrUnverified, // Fresh or FreshnessUnverified readings.
};

// "<signal_key> <comparison> <operand>", for example
// "transmission.gear Equal reverse".
struct SignalCondition {
  std::string_view signal_key{};
  Comparison comparison{Comparison::Equal};
  RuleOperand operand{};
};

// Level rule: Activate while the condition holds on an actionable reading,
// Deactivate otherwise (including every non-actionable reading). A level rule
// owns its ActionId: a second level rule on the same action is rejected.
struct StateRuleConfig {
  SignalCondition condition{};
  ActionId action{};
  FreshnessRequirement freshness{FreshnessRequirement::Fresh};
};

// Direction of the condition transition that fires an event rule.
enum class EventEdge : std::uint8_t { BecomesTrue, BecomesFalse };

// Edge rule: Trigger when the condition changes in the chosen direction
// between two consecutive actionable readings. Triggers are one-shot, so edge
// rules may share an ActionId with each other and with one level rule.
struct EventRuleConfig {
  SignalCondition condition{};
  EventEdge edge{EventEdge::BecomesTrue};
  ActionId action{};
  FreshnessRequirement freshness{FreshnessRequirement::Fresh};
};

} // namespace action_engine
