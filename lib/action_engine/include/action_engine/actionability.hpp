#pragma once

#include <optional>

#include "action_engine/rule_config.hpp"
#include "vehicle_signals/signal_contracts.hpp"

// The engine's single consumer-level availability policy, shared by state,
// event and range rules.

namespace action_engine {

// True when `availability` satisfies `requirement`. NoData, Stale and
// Unavailable never do.
[[nodiscard]] constexpr bool meets(FreshnessRequirement requirement,
                                   vehicle_signals::Availability availability) noexcept {
  using vehicle_signals::Availability;
  if (availability == Availability::Fresh) {
    return true;
  }
  return availability == Availability::FreshnessUnverified &&
         requirement == FreshnessRequirement::FreshOrUnverified;
}

// The reading's value when it is actionable: present, of type `type`, finite
// for Number, and with an availability that meets `requirement`. Otherwise
// std::nullopt.
[[nodiscard]] std::optional<vehicle_signals::SignalValue>
actionable_value(const vehicle_signals::SignalReading &reading, vehicle_signals::SignalType type,
                 FreshnessRequirement requirement) noexcept;

} // namespace action_engine
