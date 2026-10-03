#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "action_engine/rule_config.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/lighting_zone.hpp"
#include "local_argb_actions/effect_bindings.hpp"

// Version 1 persisted controller configuration model: the typed form of the
// shared YAML/JSON contract described in docs/specs/configuration/controller-config.md.
// It describes "vehicle signal -> rule -> named action -> output binding" with
// the action engine's persisted-form concepts and the local LED adapter's
// binding concepts. It owns its strings, so a loader can fill it from any
// serialized form, and it never names numeric ActionIds, GPIOs, CAN settings
// or renderer internals. validate() (validation.hpp) checks it without a
// signal catalog, parser or runtime.

namespace controller_config::persisted {

// Persisted integers keep the full range a serialized document can express, so
// that validation, rather than a loader's narrowing conversion, decides whether
// a value fits its runtime representation (a uint8 priority, a strip index).
using Integer = std::int64_t;

// The only schema version this model represents. A loader migrates an older
// document to this model before validation; any other version is rejected.
inline constexpr Integer kSchemaVersion = 1;

// A named action. Rules and output bindings refer to actions by this name; the
// runtime assigns numeric ActionIds when it applies the configuration.
struct Action {
  std::string name{};
};

// Typed right-hand side of a condition, mirroring action_engine::RuleOperand.
struct BooleanOperand {
  bool value{false};
};
struct NumberOperand {
  float value{0.0F};
};
// An enum choice key of the signal, such as "left" for vehicle.turn_state.
struct ChoiceOperand {
  std::string key{};
};
using Operand = std::variant<BooleanOperand, NumberOperand, ChoiceOperand>;

// "<signal_key> <comparison> <operand>", as action_engine::SignalCondition.
struct Condition {
  std::string signal_key{};
  action_engine::Comparison comparison{action_engine::Comparison::Equal};
  Operand operand{};
};

// Level rule on notified readings (action_engine::StateRuleConfig).
struct StateRule {
  std::string action{};
  Condition condition{};
  action_engine::FreshnessRequirement freshness{action_engine::FreshnessRequirement::Fresh};
};

// Level rule on sampled reads (action_engine::SampledStateRuleConfig), with an
// optional release threshold for hysteresis on ordered numeric conditions.
struct SampledStateRule {
  std::string action{};
  Condition condition{};
  action_engine::FreshnessRequirement freshness{action_engine::FreshnessRequirement::Fresh};
  std::optional<float> release_threshold{};
};

// One-shot edge rule (action_engine::EventRuleConfig).
struct EventRule {
  std::string action{};
  Condition condition{};
  action_engine::EventEdge edge{action_engine::EventEdge::BecomesTrue};
  action_engine::FreshnessRequirement freshness{action_engine::FreshnessRequirement::Fresh};
};

// Sampled linear level mapping (action_engine::RangeRuleConfig).
struct RangeRule {
  std::string action{};
  std::string signal_key{};
  action_engine::NumericRange input{};
  action_engine::NumericRange output{};
  action_engine::FreshnessRequirement freshness{action_engine::FreshnessRequirement::Fresh};
};

using Rule = std::variant<StateRule, SampledStateRule, EventRule, RangeRule>;

// The persisted `type` of a rule; the enumerator order is the Rule alternative
// order.
enum class RuleType : std::uint8_t { State, SampledState, Event, Range };

static_assert(std::is_same_v<std::variant_alternative_t<0, Rule>, StateRule>);
static_assert(std::is_same_v<std::variant_alternative_t<1, Rule>, SampledStateRule>);
static_assert(std::is_same_v<std::variant_alternative_t<2, Rule>, EventRule>);
static_assert(std::is_same_v<std::variant_alternative_t<3, Rule>, RangeRule>);

[[nodiscard]] constexpr RuleType type_of(const Rule &rule) noexcept {
  return static_cast<RuleType>(rule.index());
}

// A contiguous run of logical strip pixels (local_argb::internal::LedZone).
struct LedZone {
  Integer start{0};
  Integer length{0};
  local_argb::internal::FillDirection direction{local_argb::internal::FillDirection::StartToEnd};
};

// An 8-bit-per-channel colour (local_argb::internal::LightingRgb).
struct Rgb {
  Integer red{0};
  Integer green{0};
  Integer blue{0};
};

// The LED adapter's default effect priority (local_argb::internal::EffectPriority).
inline constexpr Integer kDefaultPriority = local_argb::internal::EffectPriority::kDefault;

// Binds a named action to a discrete local LED effect.
struct LedEffectBinding {
  std::string action{};
  local_argb_actions::LedEffect effect{local_argb_actions::LedEffect::LeftTurn};
  Integer priority{kDefaultPriority};
};

// Binds a named action to a level-driven local LED fill.
struct LedFillBinding {
  std::string action{};
  LedZone zone{};
  Rgb color{};
  Integer priority{kDefaultPriority};
};

// Persisted milliseconds share the renderer contract's bounded lifetime.
static_assert(local_argb::internal::kMaxTransientDurationUs >= 1000 &&
                  local_argb::internal::kMaxTransientDurationUs % 1000 == 0,
              "Transient duration maximum must be at least one whole millisecond");
inline constexpr Integer kMaxTransientDurationMs =
    static_cast<Integer>(local_argb::internal::kMaxTransientDurationUs / 1000);

// Binds Trigger to a solid local LED zone for a finite duration in milliseconds.
struct LedTransientBinding {
  std::string action{};
  LedZone zone{};
  Rgb color{};
  Integer duration_ms{0};
  Integer priority{kDefaultPriority};
};

// Binds a state action to a solid local LED zone: Activate lights the whole
// zone in `color`; Deactivate and fail-off clear it. Only state and sampled
// state rules may drive the action. The zone direction does not affect drawing.
struct LedSolidBinding {
  std::string action{};
  LedZone zone{};
  Rgb color{};
  Integer priority{kDefaultPriority};
};

using OutputBinding =
    std::variant<LedEffectBinding, LedFillBinding, LedTransientBinding, LedSolidBinding>;

// The persisted `type` of an output binding; the enumerator order is the
// OutputBinding alternative order.
enum class OutputType : std::uint8_t { LedEffect, LedFill, LedTransient, LedSolid };

static_assert(std::is_same_v<std::variant_alternative_t<0, OutputBinding>, LedEffectBinding>);
static_assert(std::is_same_v<std::variant_alternative_t<1, OutputBinding>, LedFillBinding>);
static_assert(std::is_same_v<std::variant_alternative_t<2, OutputBinding>, LedTransientBinding>);
static_assert(std::is_same_v<std::variant_alternative_t<3, OutputBinding>, LedSolidBinding>);

[[nodiscard]] constexpr OutputType type_of(const OutputBinding &binding) noexcept {
  return static_cast<OutputType>(binding.index());
}

// The top-level persisted document.
struct ControllerConfig {
  Integer version{kSchemaVersion};
  std::vector<Action> actions{};
  std::vector<Rule> rules{};
  std::vector<OutputBinding> outputs{};
};

} // namespace controller_config::persisted
