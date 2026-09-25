#pragma once

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace mazda {

class VehicleTelemetry;

// Concrete generic-signal provider over the Mazda telemetry facade. It exposes
// the Mazda-owned static signal catalog and value-only vehicle_signals
// readings/notifications; Mazda state, CAN identifiers, enums, notification
// channels and descriptors stay implementation-only. It implements the generic
// vehicle_signals::SignalProvider port, so application composition can hand it
// to a make-independent consumer that never includes this header. The facade
// delivers notifications serially on its single dispatcher context, as the
// port requires.
//
// The provider is a stateless, non-owning view: the facade must outlive it,
// and destroying the provider is safe at any time, including while the facade
// is running. Generic registrations belong to the facade exactly like typed
// ones: they share the facade's fixed per-channel subscriber capacity and are
// released by unsubscribe() or by destroying the facade, never by destroying
// the provider. Subscription tokens are facade-scoped, so any provider over
// the same facade may unsubscribe a token another one issued.
//
// subscribe() and unsubscribe() are lifecycle mutations of the facade: they
// must run on the facade's lifecycle-owner host thread or ESP-IDF task and
// are accepted only while the facade is stopped; otherwise they fail with
// SignalStatus::InvalidState. read() and catalog() are safe from any context.
//
// Callbacks receive value-only SignalNotification copies on the facade's
// dispatcher context. As with the typed API, the callback context pointer and
// any pointed-to storage are borrowed until unsubscribe() succeeds, a facade
// stop() returns successfully, or the facade is destroyed; after a failed or
// timed-out stop, callbacks can still be active. A callback must not start,
// stop, subscribe or unsubscribe on the facade or any provider over it.
class MazdaSignalProvider final : public vehicle_signals::SignalProvider {
public:
  explicit MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept;

  MazdaSignalProvider(const MazdaSignalProvider &) = delete;
  MazdaSignalProvider &operator=(const MazdaSignalProvider &) = delete;
  MazdaSignalProvider(MazdaSignalProvider &&) = delete;
  MazdaSignalProvider &operator=(MazdaSignalProvider &&) = delete;

  // Non-owning view over the static catalog; valid for the program lifetime.
  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept override;

  // Latest-state read of one catalog signal. InvalidSignal or
  // UnsupportedCapability are request failures, distinct from a successful
  // reading whose availability is NoData.
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalReading>
  read(vehicle_signals::SignalId id) const noexcept;

  // Stopped-only subscription to a Notify-capable catalog signal.
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>
  subscribe(vehicle_signals::SignalId id, vehicle_signals::SignalCallback callback,
            void *context) noexcept override;
  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(vehicle_signals::SignalSubscription subscription) noexcept override;

private:
  VehicleTelemetry *telemetry_{nullptr};
};

} // namespace mazda
