#pragma once

#include <cstdint>

namespace action_engine {

// Outcome of configuring the engine (adding a sink or a rule). Every failure
// leaves the engine unchanged.
enum class ConfigStatus : std::uint8_t {
  Ok,
  InvalidState,          // Configuration is accepted only while detached.
  CapacityExceeded,      // The fixed sink or rule capacity is full.
  DuplicateSink,         // The sink is already registered.
  InvalidAction,         // The rule's ActionId is zero.
  DuplicateAction,       // A level (state, range or sampled state) rule already drives it.
  UnknownSignal,         // The signal key is not in the provider catalog.
  UnsupportedCapability, // The signal cannot notify (or, for polled rules, be read).
  TypeMismatch,          // The operand type differs from the signal type.
  UnknownChoice,         // The enum choice key is not a choice of the signal.
  InvalidOperand,        // A Number operand is NaN or infinite.
  UnsupportedComparison, // An ordered comparison on a non-Number signal.
  InvalidRange,          // A non-finite range bound or span, or input.from >= input.to.
  InvalidHysteresis,     // A sampled threshold's release boundary is invalid.
};

} // namespace action_engine
