#pragma once

#include <vector>

#include "replay/signal_observer.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace replay {

// Connects a SignalProvider to zero or more SignalObservers. attach()
// publishes the catalog and subscribes once per Notify-capable signal;
// notifications are forwarded unchanged at the clock's current time.
// sample() reads every polled (Read but not Notify) signal in catalog order.
// With no observers, it neither subscribes nor reads, so the provider is
// untouched. attach() and detach() must run while the provider is stopped.
class SignalObserverFanOut final {
public:
  SignalObserverFanOut(vehicle_signals::SignalProvider &provider,
                       const vehicle_core::MonotonicClock &clock,
                       SignalObservers observers) noexcept;

  SignalObserverFanOut(const SignalObserverFanOut &) = delete;
  SignalObserverFanOut &operator=(const SignalObserverFanOut &) = delete;
  SignalObserverFanOut(SignalObserverFanOut &&) = delete;
  SignalObserverFanOut &operator=(SignalObserverFanOut &&) = delete;

  // False when an observer is null.
  [[nodiscard]] bool configured() const noexcept;
  // Rolls back every subscription it made when one fails.
  [[nodiscard]] bool attach() noexcept;
  [[nodiscard]] bool sample() const noexcept;
  [[nodiscard]] bool detach() noexcept;
  [[nodiscard]] bool attached() const noexcept { return attached_; }

private:
  static void forward(void *context,
                      const vehicle_signals::SignalNotification &notification) noexcept;
  void publish(const vehicle_signals::SignalMetadata &signal,
               const vehicle_signals::SignalReading &reading) const noexcept;
  [[nodiscard]] bool subscribe_notified_signals() noexcept;

  vehicle_signals::SignalProvider *provider_;
  const vehicle_core::MonotonicClock *clock_;
  SignalObservers observers_;
  std::vector<vehicle_signals::SignalSubscription> subscriptions_{};
  bool attached_{false};
};

} // namespace replay
