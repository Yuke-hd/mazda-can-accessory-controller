#pragma once

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace vehicle_signals {

// Generic provider port: the minimal surface a make-independent consumer
// needs to resolve catalog keys and receive latest-state notices. Concrete
// providers bind it to their own telemetry; consumers depend only on this
// interface, so they never learn the make, CAN identifiers or channels.
//
// The port inherits the callback contract from signal_contracts.hpp:
// - subscribe() and unsubscribe() are lifecycle mutations accepted only while
//   the provider is stopped, on its lifecycle owner; otherwise they fail with
//   SignalStatus::InvalidState.
// - The callback context and any pointed-to storage are borrowed until
//   unsubscribe() succeeds or a provider stop succeeds.
// - A callback must not start, stop, subscribe or unsubscribe on the provider
//   that invoked it.
// In addition, a provider delivers notifications serially from one dispatcher
// context, so a consumer never sees two concurrent callbacks from the same
// provider.
//
// The port does not own or destroy providers: the destructor is protected and
// non-virtual, and the concrete provider's owner controls its lifetime.
class SignalProvider {
public:
  SignalProvider(const SignalProvider &) = delete;
  SignalProvider &operator=(const SignalProvider &) = delete;
  SignalProvider(SignalProvider &&) = delete;
  SignalProvider &operator=(SignalProvider &&) = delete;

  // Non-owning view over the provider's static catalog.
  [[nodiscard]] virtual SignalCatalogView catalog() const noexcept = 0;

  // Stopped-only subscription to a Notify-capable catalog signal.
  [[nodiscard]] virtual SignalResult<SignalSubscription>
  subscribe(SignalId id, SignalCallback callback, void *context) noexcept = 0;
  [[nodiscard]] virtual SignalStatusResult
  unsubscribe(SignalSubscription subscription) noexcept = 0;

protected:
  SignalProvider() noexcept = default;
  ~SignalProvider() = default;
};

} // namespace vehicle_signals
