#include "action_engine/actionability.hpp"

#include <cmath>

namespace action_engine {

std::optional<vehicle_signals::SignalValue>
actionable_value(const vehicle_signals::SignalReading &reading, vehicle_signals::SignalType type,
                 FreshnessRequirement requirement) noexcept {
  if (!reading.value.has_value() || reading.value->type() != type ||
      !meets(requirement, reading.availability)) {
    return std::nullopt;
  }
  const auto number = reading.value->as_number();
  if (number.has_value() && !std::isfinite(*number)) {
    return std::nullopt;
  }
  return reading.value;
}

} // namespace action_engine
