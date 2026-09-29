#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "action_engine/condition.hpp"
#include "action_engine/range_rule.hpp"
#include "action_engine/rule_config.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb/lighting_zone.hpp"
#include "local_argb_actions/effect_bindings.hpp"

// This header is the owning, parser-independent representation of a persisted
// controller configuration. It deliberately contains no GPIO, driver, task,
// or renderer-lifecycle settings. The JSON loader populates this model,
// validates it, and then hands the string views produced by
// to_action_engine_rule() to the action engine while this object remains
// alive.

namespace controller_config {

inline constexpr std::uint32_t kControllerConfigVersion = 1U;
inline constexpr std::size_t kLogicalStripPixelCount = 100U;

enum class SchemaError : std::uint8_t {
  None,
  UnsupportedVersion,
  EmptyActionName,
  DuplicateActionName,
  ActionIdExhausted,
  UnknownAction,
  EmptySignal,
  UnknownRuleType,
  UnknownComparison,
  UnknownFreshness,
  UnknownEventEdge,
  UnknownOperandKind,
  EmptyChoice,
  NonFiniteNumber,
  OperandComparisonMismatch,
  InvalidRange,
  InvalidHysteresis,
  UnknownSignal,
  UnsupportedCapability,
  TypeMismatch,
  UnknownChoice,
  UnsupportedComparison,
  UnknownLedEffect,
  UnknownFillDirection,
  InvalidLedZone,
  InvalidRgb,
  InvalidPriority,
};

struct ValidationResult final {
  SchemaError error{SchemaError::None};
  std::size_t index{0};

  [[nodiscard]] bool ok() const noexcept { return error == SchemaError::None; }
};

// Persisted action names are the references used by rules and LED bindings.
// Runtime ActionIds are assigned deterministically from declaration order
// (one-based) by action_id(); numeric ActionIds never cross the persistence
// boundary.
struct ActionSpec final {
  std::string name{};
};
using NamedAction = ActionSpec;

enum class OperandKind : std::uint8_t { Boolean, Number, Choice };

struct OperandSpec final {
  OperandKind kind{OperandKind::Boolean};
  bool boolean_value{false};
  float number_value{0.0F};
  std::string choice_value{};

  [[nodiscard]] static OperandSpec boolean(const bool value) {
    OperandSpec operand{};
    operand.kind = OperandKind::Boolean;
    operand.boolean_value = value;
    return operand;
  }
  [[nodiscard]] static OperandSpec number(const float value) {
    OperandSpec operand{};
    operand.kind = OperandKind::Number;
    operand.number_value = value;
    return operand;
  }
  [[nodiscard]] static OperandSpec choice(std::string value) {
    OperandSpec operand{};
    operand.kind = OperandKind::Choice;
    operand.choice_value = std::move(value);
    return operand;
  }
};

// Rule and enum fields remain strings so a future parser can preserve the
// persisted spelling until validation. Conversion accepts the canonical
// snake_case names documented in docs/development/controller-config-schema.md.
struct RuleSpec final {
  std::string type{};       // state, sampled_state, event, range
  std::string action{};     // ActionSpec::name
  std::string signal{};     // signal catalog key
  std::string comparison{}; // equal, not_equal, less, less_or_equal, greater, greater_or_equal
  OperandSpec operand{};
  std::string freshness{"fresh"};                 // fresh, fresh_or_unverified
  std::string edge{"becomes_true"};               // event only
  std::optional<float> release_threshold{};       // sampled state only
  action_engine::NumericRange input{};            // range only
  action_engine::NumericRange output{0.0F, 1.0F}; // range only
};

struct EffectBindingSpec final {
  std::string action{};
  std::string effect{}; // left_turn, right_turn, brake
  int priority{100};
};

struct RgbSpec final {
  int red{0};
  int green{0};
  int blue{0};
};

struct ZoneSpec final {
  std::size_t start{0};
  std::size_t length{0};
  std::string direction{}; // start_to_end, end_to_start, center_out
};

struct FillBindingSpec final {
  std::string action{};
  ZoneSpec zone{};
  RgbSpec color{};
  int priority{100};
};

struct Configuration final {
  std::uint32_t version{kControllerConfigVersion};
  std::vector<ActionSpec> actions{};
  std::vector<RuleSpec> rules{};
  std::vector<EffectBindingSpec> effect_bindings{};
  std::vector<FillBindingSpec> fill_bindings{};

  // Returns the runtime identity assigned to `name`; declaration order is
  // the only source of the non-persisted numeric id. No allocation is
  // performed.
  [[nodiscard]] std::optional<action_engine::ActionId>
  action_id(std::string_view name) const noexcept;
};
using PersistedConfiguration = Configuration;
using ControllerConfig = Configuration;

[[nodiscard]] std::optional<action_engine::Comparison>
comparison_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<action_engine::FreshnessRequirement>
freshness_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<action_engine::EventEdge>
event_edge_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<OperandKind> operand_kind_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<local_argb_actions::LedEffect>
led_effect_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<local_argb::internal::FillDirection>
fill_direction_from_name(std::string_view name) noexcept;

// Runtime rules retain string views into `configuration`. The caller must
// keep the owning Configuration alive and must not mutate its string storage
// while these values are being used by the action engine.
using EngineRule =
    std::variant<action_engine::StateRuleConfig, action_engine::SampledStateRuleConfig,
                 action_engine::EventRuleConfig, action_engine::RangeRuleConfig>;

[[nodiscard]] std::optional<EngineRule> to_action_engine_rule(const Configuration &configuration,
                                                              std::size_t rule_index) noexcept;

[[nodiscard]] ValidationResult validate(const Configuration &configuration) noexcept;
[[nodiscard]] ValidationResult validate(const Configuration &configuration,
                                        vehicle_signals::SignalCatalogView catalog) noexcept;

// The complete production profile currently represented by LightingProfile.
// It is returned by value so all persisted strings are owned by the caller.
[[nodiscard]] Configuration default_configuration();

} // namespace controller_config
