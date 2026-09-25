#pragma once

#include <array>
#include <cstddef>

#include "action_engine/rule_set.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace action_engine {

// The engine's live provider registrations, at most one per distinct signal:
// provider channels have few subscriber slots, so rules on the same signal
// share one subscription. Provider mutations follow the port's stopped-only
// lifecycle contract.
class SubscriptionSet final {
public:
  static constexpr std::size_t kCapacity = RuleSet::kCapacity;

  // Subscribes to `signal` unless it is already subscribed (then Ok).
  // Returns the provider's failure, or CapacityExceeded when full.
  [[nodiscard]] vehicle_signals::SignalStatus subscribe(vehicle_signals::SignalProvider &provider,
                                                        vehicle_signals::SignalId signal,
                                                        vehicle_signals::SignalCallback callback,
                                                        void *context) noexcept;
  // Unsubscribes newest first. On a provider failure it stops and keeps the
  // failed registration and every older one, so the call can be retried.
  [[nodiscard]] vehicle_signals::SignalStatus
  unsubscribe_all(vehicle_signals::SignalProvider &provider) noexcept;

  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] bool contains(vehicle_signals::SignalId signal) const noexcept;

private:
  struct Registration {
    vehicle_signals::SignalId signal{};
    vehicle_signals::SignalSubscription subscription{};
  };

  std::array<Registration, kCapacity> registrations_{};
  std::size_t count_{0};
};

} // namespace action_engine
