#pragma once

#include <cstdint>

#include "vehicle_core/time.hpp"

namespace mazda {

enum class SelectorPosition : std::uint8_t {
  Unknown,
  Shifting,
  Park,
  Reverse,
  Neutral,
  Drive,
};

enum class ActualGear : std::uint8_t {
  Unknown,
  // Raw zero is the source DBC's combined P_or_N state. Keep Park as a
  // distinct compatibility value; no current source value decodes to it.
  ParkOrNeutral,
  Park,
  Neutral,
  Reverse,
  First,
  Second,
  Third,
  Fourth,
  Fifth,
  Sixth,
  // Raw 15 is explicitly labelled Shifting by the source DBC.
  Shifting,
};

enum class FrontWiperPosition : std::uint8_t { Unknown, Off, On, High, Intermittent };

enum class TurnState : std::uint8_t { Unknown, Off, Left, Right, Hazard };

enum class TurnEventType : std::uint8_t { StateChanged };

// Semantic event: intentionally contains no Mazda CAN identifier or raw
// payload. It is safe to pass to a dashboard or effect consumer.
struct TurnEdgeEvent {
  TurnEventType type{TurnEventType::StateChanged};
  TurnState previous{TurnState::Unknown};
  TurnState current{TurnState::Unknown};
  vehicle_core::MonotonicTimestamp timestamp_us{0};
};

} // namespace mazda
