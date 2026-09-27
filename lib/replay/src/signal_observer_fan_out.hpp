#pragma once

#include <vector>

#include "replay/signal_observer.hpp"
#include "vehicle_core/time.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace replay {

// Connects a SignalProvider to zero or more SignalObservers. attach()
// subscribes once per Notify-capable signal; open() then publishes the
// catalog, so observers hear nothing from a replay that never starts.
// Notifications are stamped with the clock's current time; those arriving
// between attach() and open() are held and delivered by open(), in order and
// with their original times, and later ones are forwarded directly.
// sample() reads every polled (Read but not Notify) signal in catalog order.
// With no observers, it neither subscribes nor reads, so the provider is
// untouched. attach() and detach() must run while the provider is stopped.
// Notifications may arrive on the provider's thread; the owner must order
// them before open() and detach() (ReplayController drains them first).
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
  // Publishes the catalog and the held readings once per attach().
  void open() noexcept;
  [[nodiscard]] bool sample() const noexcept;
  [[nodiscard]] bool detach() noexcept;
  [[nodiscard]] bool attached() const noexcept { return attached_; }

private:
  struct HeldReading {
    vehicle_core::MonotonicTimestamp time_us;
    const vehicle_signals::SignalMetadata *signal;
    vehicle_signals::SignalReading reading;
  };

  static void forward(void *context,
                      const vehicle_signals::SignalNotification &notification) noexcept;
  void receive(const vehicle_signals::SignalMetadata &signal,
               const vehicle_signals::SignalReading &reading) noexcept;
  void publish(vehicle_core::MonotonicTimestamp now_us,
               const vehicle_signals::SignalMetadata &signal,
               const vehicle_signals::SignalReading &reading) const noexcept;
  [[nodiscard]] bool subscribe_notified_signals() noexcept;

  vehicle_signals::SignalProvider *provider_;
  const vehicle_core::MonotonicClock *clock_;
  SignalObservers observers_;
  std::vector<vehicle_signals::SignalSubscription> subscriptions_{};
  std::vector<HeldReading> held_{};
  bool attached_{false};
  bool opened_{false};
};

} // namespace replay
