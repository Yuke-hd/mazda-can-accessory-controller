#pragma once

#include <cstddef>
#include <cstdint>

#include "controller_config/persisted/model.hpp"

// Schema-level validation of a persisted controller configuration. It needs no
// parser, signal catalog or runtime: it checks structure, cross-references and
// the brake freshness safety policy and value constraints enforced when a
// configuration is applied. Checks that need the provider catalog (an unknown
// signal key, a signal/operand type mismatch, an unknown choice key, a signal
// capability) and fixed runtime capacities remain with the runtime apply step.

namespace controller_config::persisted {

// Where a validation error was found.
enum class ConfigSection : std::uint8_t { Document, Actions, Rules, Outputs };

// Names match action_engine::ConfigStatus and local_argb_actions::BindingStatus
// where the constraint is the same one.
enum class ValidationError : std::uint8_t {
  None,
  UnsupportedVersion,    // version is not kSchemaVersion.
  EmptyActionName,       // An action name is empty.
  DuplicateActionName,   // An action name is declared twice.
  UndeclaredAction,      // A rule or binding names an action not in `actions`.
  DuplicateAction,       // A second level (state, sampled state, range) rule drives an action.
  EmptySignalKey,        // A rule's signal key is empty.
  UnknownComparison,     // The comparison is not a recognized value.
  UnknownFreshness,      // The freshness requirement is not a recognized value.
  UnknownEventEdge,      // The event edge is not a recognized value.
  EmptyChoice,           // A choice operand's key is empty.
  InvalidOperand,        // A number operand is NaN or infinite.
  UnsupportedComparison, // An ordered comparison with a non-number operand.
  InvalidHysteresis,     // A release threshold is invalid for its condition.
  InvalidRange,          // A non-finite range bound or span, or input.from >= input.to.
  UnknownLedEffect,      // The LED effect is not a recognized value.
  UnknownFillDirection,  // The zone's fill direction is not a recognized value.
  EmptyZone,             // The zone's length is zero.
  ZoneOutOfRange,        // The zone does not fit inside the logical strip.
  InvalidColor,          // A colour channel is outside 0..255.
  InvalidPriority,       // The priority is outside 0..255.
  DuplicateBinding,      // The same action already drives this effect or zone.
  // A known freshness requirement violates signal safety policy.
  UnsupportedFreshnessPolicy,
};

// The first error found, checking the version, then actions, rules and
// outputs in document order. `index` is the entry's position in its section
// (0 for Document).
struct ValidationResult {
  ValidationError error{ValidationError::None};
  ConfigSection section{ConfigSection::Document};
  std::size_t index{0};

  [[nodiscard]] bool ok() const noexcept { return error == ValidationError::None; }
};

[[nodiscard]] ValidationResult validate(const ControllerConfig &config) noexcept;

} // namespace controller_config::persisted
