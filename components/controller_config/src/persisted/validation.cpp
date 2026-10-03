#include "controller_config/persisted/validation.hpp"

#include <cmath>
#include <string_view>
#include <vector>

#include "controller_config/persisted/names.hpp"
#include "local_argb/pixel_frame.hpp"

// Mirrors the constraints action_engine::ActionEngine and
// local_argb_actions::LedActionSink enforce at apply time, so a document that
// validates here fails at runtime only for catalog or capacity reasons. The
// checks scan earlier entries instead of building sets: documents are small and
// validation stays allocation-free.

namespace controller_config::persisted {
namespace {

using action_engine::Comparison;
using action_engine::NumericRange;

constexpr Integer kMaxByte = 255;

[[nodiscard]] bool is_byte(const Integer value) noexcept { return value >= 0 && value <= kMaxByte; }

[[nodiscard]] bool is_declared(const std::vector<Action> &actions,
                               const std::string_view name) noexcept {
  for (const Action &action : actions) {
    if (action.name == name)
      return true;
  }
  return false;
}

// --- Actions ---------------------------------------------------------------

[[nodiscard]] ValidationError action_error(const std::vector<Action> &actions,
                                           const std::size_t index) noexcept {
  const std::string &name = actions[index].name;
  if (name.empty())
    return ValidationError::EmptyActionName;
  for (std::size_t earlier = 0; earlier < index; ++earlier) {
    if (actions[earlier].name == name)
      return ValidationError::DuplicateActionName;
  }
  return ValidationError::None;
}

// --- Rules -----------------------------------------------------------------

[[nodiscard]] bool is_ordered(const Comparison comparison) noexcept {
  return comparison == Comparison::Less || comparison == Comparison::LessOrEqual ||
         comparison == Comparison::Greater || comparison == Comparison::GreaterOrEqual;
}

[[nodiscard]] ValidationError operand_error(const Operand &operand) noexcept {
  if (const auto *choice = std::get_if<ChoiceOperand>(&operand))
    return choice->key.empty() ? ValidationError::EmptyChoice : ValidationError::None;
  if (const auto *number = std::get_if<NumberOperand>(&operand))
    return std::isfinite(number->value) ? ValidationError::None : ValidationError::InvalidOperand;
  return ValidationError::None;
}

[[nodiscard]] ValidationError condition_error(const Condition &condition) noexcept {
  if (condition.signal_key.empty())
    return ValidationError::EmptySignalKey;
  if (!name_of(condition.comparison).has_value())
    return ValidationError::UnknownComparison;
  if (const ValidationError error = operand_error(condition.operand);
      error != ValidationError::None)
    return error;
  if (is_ordered(condition.comparison) && !std::holds_alternative<NumberOperand>(condition.operand))
    return ValidationError::UnsupportedComparison;
  return ValidationError::None;
}

// As action_engine: the release threshold must be finite and lie on the
// inactive side of a finite ordered numeric activation threshold.
[[nodiscard]] bool is_valid_release(const Condition &condition, const float release) noexcept {
  const auto *activation = std::get_if<NumberOperand>(&condition.operand);
  if (activation == nullptr || !std::isfinite(release))
    return false;
  switch (condition.comparison) {
  case Comparison::Greater:
  case Comparison::GreaterOrEqual:
    return release < activation->value;
  case Comparison::Less:
  case Comparison::LessOrEqual:
    return release > activation->value;
  case Comparison::Equal:
  case Comparison::NotEqual:
    return false;
  }
  return false;
}

// As action_engine::RangeRule: finite bounds and span, ascending input.
[[nodiscard]] bool is_finite(const NumericRange range) noexcept {
  return std::isfinite(range.from) && std::isfinite(range.to) &&
         std::isfinite(range.to - range.from);
}

[[nodiscard]] ValidationError
freshness_error(const action_engine::FreshnessRequirement freshness) noexcept {
  return name_of(freshness).has_value() ? ValidationError::None : ValidationError::UnknownFreshness;
}

[[nodiscard]] ValidationError body_error(const StateRule &rule) noexcept {
  if (const ValidationError error = condition_error(rule.condition); error != ValidationError::None)
    return error;
  return freshness_error(rule.freshness);
}

[[nodiscard]] ValidationError body_error(const SampledStateRule &rule) noexcept {
  if (const ValidationError error = condition_error(rule.condition); error != ValidationError::None)
    return error;
  if (const ValidationError error = freshness_error(rule.freshness); error != ValidationError::None)
    return error;
  if (rule.release_threshold.has_value() &&
      !is_valid_release(rule.condition, *rule.release_threshold))
    return ValidationError::InvalidHysteresis;
  return ValidationError::None;
}

[[nodiscard]] ValidationError body_error(const EventRule &rule) noexcept {
  if (const ValidationError error = condition_error(rule.condition); error != ValidationError::None)
    return error;
  if (!name_of(rule.edge).has_value())
    return ValidationError::UnknownEventEdge;
  return freshness_error(rule.freshness);
}

[[nodiscard]] ValidationError body_error(const RangeRule &rule) noexcept {
  if (rule.signal_key.empty())
    return ValidationError::EmptySignalKey;
  if (const ValidationError error = freshness_error(rule.freshness); error != ValidationError::None)
    return error;
  if (!is_finite(rule.input) || !is_finite(rule.output) || !(rule.input.from < rule.input.to))
    return ValidationError::InvalidRange;
  return ValidationError::None;
}

[[nodiscard]] const std::string &action_of(const Rule &rule) noexcept {
  return std::visit(
      [](const auto &alternative) -> const std::string & { return alternative.action; }, rule);
}

// State, sampled state and range rules own their action's level; event rules
// only pulse it.
[[nodiscard]] bool is_level(const Rule &rule) noexcept { return type_of(rule) != RuleType::Event; }

[[nodiscard]] bool has_earlier_level_rule(const std::vector<Rule> &rules,
                                          const std::size_t index) noexcept {
  const std::string &action = action_of(rules[index]);
  for (std::size_t earlier = 0; earlier < index; ++earlier) {
    if (is_level(rules[earlier]) && action_of(rules[earlier]) == action)
      return true;
  }
  return false;
}

[[nodiscard]] ValidationError rule_error(const ControllerConfig &config,
                                         const std::size_t index) noexcept {
  const Rule &rule = config.rules[index];
  if (!is_declared(config.actions, action_of(rule)))
    return ValidationError::UndeclaredAction;
  if (const ValidationError error =
          std::visit([](const auto &alternative) { return body_error(alternative); }, rule);
      error != ValidationError::None)
    return error;
  if (is_level(rule) && has_earlier_level_rule(config.rules, index))
    return ValidationError::DuplicateAction;
  return ValidationError::None;
}

// --- Outputs ---------------------------------------------------------------

// As local_argb's renderer: a recognized direction, a non-empty length, and a
// zone that fits inside the logical strip.
[[nodiscard]] ValidationError zone_error(const LedZone &zone) noexcept {
  constexpr auto kStripLength = static_cast<Integer>(local_argb::kLedCount);
  if (!name_of(zone.direction).has_value())
    return ValidationError::UnknownFillDirection;
  if (zone.length == 0)
    return ValidationError::EmptyZone;
  if (zone.start < 0 || zone.length < 0 || zone.start >= kStripLength ||
      zone.length > kStripLength - zone.start)
    return ValidationError::ZoneOutOfRange;
  return ValidationError::None;
}

[[nodiscard]] ValidationError body_error(const LedEffectBinding &binding) noexcept {
  if (!name_of(binding.effect).has_value())
    return ValidationError::UnknownLedEffect;
  if (!is_byte(binding.priority))
    return ValidationError::InvalidPriority;
  return ValidationError::None;
}

template <typename Binding>
[[nodiscard]] ValidationError zone_binding_error(const Binding &binding) noexcept {
  if (const ValidationError error = zone_error(binding.zone); error != ValidationError::None)
    return error;
  if (!is_byte(binding.color.red) || !is_byte(binding.color.green) || !is_byte(binding.color.blue))
    return ValidationError::InvalidColor;
  if (!is_byte(binding.priority))
    return ValidationError::InvalidPriority;
  return ValidationError::None;
}

[[nodiscard]] ValidationError body_error(const LedFillBinding &binding) noexcept {
  return zone_binding_error(binding);
}

[[nodiscard]] ValidationError body_error(const LedTransientBinding &binding) noexcept {
  if (const auto error = zone_binding_error(binding); error != ValidationError::None)
    return error;
  if (binding.duration_ms <= 0 || binding.duration_ms > kMaxTransientDurationMs)
    return ValidationError::InvalidDuration;
  return ValidationError::None;
}

[[nodiscard]] ValidationError body_error(const LedSolidBinding &binding) noexcept {
  return zone_binding_error(binding);
}

// A solid zone is only on or off, so its action must come from state or
// sampled state rules: a range level or an event pulse has no solid meaning.
[[nodiscard]] bool drives_state_only(const std::vector<Rule> &rules,
                                     const std::string_view action) noexcept {
  for (const Rule &rule : rules) {
    const RuleType type = type_of(rule);
    if (action_of(rule) == action && (type == RuleType::Range || type == RuleType::Event))
      return false;
  }
  return true;
}

template <typename Binding>
[[nodiscard]] ValidationError action_kind_error(const std::vector<Rule> &,
                                                const Binding &) noexcept {
  return ValidationError::None;
}

[[nodiscard]] ValidationError action_kind_error(const std::vector<Rule> &rules,
                                                const LedSolidBinding &binding) noexcept {
  return drives_state_only(rules, binding.action) ? ValidationError::None
                                                  : ValidationError::IncompatibleActionKind;
}

// As local_argb_actions: one binding per kind, action and effect or zone
// (fill direction included). Transient and solid direction does not change the
// physical target. Appearance, priority and duration do not change a target.
[[nodiscard]] bool same_target(const LedEffectBinding &left,
                               const LedEffectBinding &right) noexcept {
  return left.action == right.action && left.effect == right.effect;
}

[[nodiscard]] bool same_target(const LedTransientBinding &left,
                               const LedTransientBinding &right) noexcept {
  return left.action == right.action && left.zone.start == right.zone.start &&
         left.zone.length == right.zone.length;
}

[[nodiscard]] bool same_target(const LedSolidBinding &left, const LedSolidBinding &right) noexcept {
  return left.action == right.action && left.zone.start == right.zone.start &&
         left.zone.length == right.zone.length;
}

[[nodiscard]] bool same_target(const LedFillBinding &left, const LedFillBinding &right) noexcept {
  return left.action == right.action && left.zone.start == right.zone.start &&
         left.zone.length == right.zone.length && left.zone.direction == right.zone.direction;
}

// Solid zones are bound as fills, so the fill identity applies across both kinds.
[[nodiscard]] bool same_target(const LedFillBinding &left, const LedSolidBinding &right) noexcept {
  return left.action == right.action && left.zone.start == right.zone.start &&
         left.zone.length == right.zone.length && left.zone.direction == right.zone.direction;
}

[[nodiscard]] bool same_target(const LedSolidBinding &left, const LedFillBinding &right) noexcept {
  return same_target(right, left);
}

template <typename Left, typename Right>
[[nodiscard]] bool same_target(const Left &, const Right &) noexcept {
  return false;
}

template <typename Binding>
[[nodiscard]] bool has_earlier_binding(const std::vector<OutputBinding> &outputs,
                                       const std::size_t index, const Binding &binding) noexcept {
  for (std::size_t earlier = 0; earlier < index; ++earlier) {
    const bool duplicate = std::visit(
        [&binding](const auto &other) { return same_target(other, binding); }, outputs[earlier]);
    if (duplicate)
      return true;
  }
  return false;
}

[[nodiscard]] ValidationError output_error(const ControllerConfig &config,
                                           const std::size_t index) noexcept {
  return std::visit(
      [&](const auto &binding) {
        if (!is_declared(config.actions, binding.action))
          return ValidationError::UndeclaredAction;
        if (const ValidationError error = body_error(binding); error != ValidationError::None)
          return error;
        if (const ValidationError error = action_kind_error(config.rules, binding);
            error != ValidationError::None)
          return error;
        if (has_earlier_binding(config.outputs, index, binding))
          return ValidationError::DuplicateBinding;
        return ValidationError::None;
      },
      config.outputs[index]);
}

// --- Document --------------------------------------------------------------

template <typename Entry, typename EntryError>
[[nodiscard]] ValidationResult first_error(const std::vector<Entry> &entries,
                                           const ConfigSection section,
                                           EntryError entry_error) noexcept {
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (const ValidationError error = entry_error(index); error != ValidationError::None)
      return ValidationResult{error, section, index};
  }
  return ValidationResult{};
}

} // namespace

ValidationResult validate(const ControllerConfig &config) noexcept {
  if (config.version != kSchemaVersion)
    return ValidationResult{ValidationError::UnsupportedVersion, ConfigSection::Document, 0};
  if (const ValidationResult result =
          first_error(config.actions, ConfigSection::Actions,
                      [&](const std::size_t index) { return action_error(config.actions, index); });
      !result.ok())
    return result;
  if (const ValidationResult result =
          first_error(config.rules, ConfigSection::Rules,
                      [&](const std::size_t index) { return rule_error(config, index); });
      !result.ok())
    return result;
  return first_error(config.outputs, ConfigSection::Outputs,
                     [&](const std::size_t index) { return output_error(config, index); });
}

} // namespace controller_config::persisted
