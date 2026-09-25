#pragma once

#include <cstdint>

namespace action_engine {

// Outcome of configuring the engine (adding a sink or a rule). Every failure
// leaves the engine unchanged.
enum class ConfigStatus : std::uint8_t {
  Ok,
  InvalidState,          // Configuration is accepted only while detached.
  CapacityExceeded,      // The fixed sink or rule capacity is full.
  InvalidAction,         // The rule's ActionId is zero.
  UnknownSignal,         // The signal key is not in the provider catalog.
  UnsupportedCapability, // The signal cannot notify.
  TypeMismatch,          // The operand type differs from the signal type.
  UnknownChoice,         // The enum choice key is not a choice of the signal.
  InvalidOperand,        // A Number operand is NaN or infinite.
  UnsupportedComparison, // An ordered comparison on a non-Number signal.
};

} // namespace action_engine
