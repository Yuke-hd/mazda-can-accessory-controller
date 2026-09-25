#pragma once

#include <cstddef>

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace mazda {

class VehicleTelemetry;

// Concrete generic-signal provider over the Mazda telemetry facade. It exposes
// the Mazda-owned static signal catalog and value-only vehicle_signals
// readings/notifications; Mazda state, CAN identifiers, enums, notification
// channels and descriptors stay implementation-only.
//
// The provider does not own the facade. The facade must outlive the provider,
// and the provider must outlive every generic subscription it issued (until a
// successful facade stop). The provider shares the facade's lifecycle owner:
// subscribe() and unsubscribe() are lifecycle mutations of that facade, must
// run on the facade's lifecycle-owner host thread or ESP-IDF task, and are
// accepted only while the facade is stopped; otherwise they fail with
// SignalStatus::InvalidState. read() and catalog() are safe from any context.
//
// Callbacks receive value-only SignalNotification copies on the facade's
// dispatcher context. The callback context pointer and any pointed-to storage
// are borrowed until the facade's stop() returns successfully; after a failed
// or timed-out stop, callbacks can still be active. A callback must not start,
// stop, subscribe or unsubscribe on the facade or this provider. Generic and
// typed subscribers share the facade's fixed per-channel subscriber capacity.
class MazdaSignalProvider final {
public:
  explicit MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept;

  MazdaSignalProvider(const MazdaSignalProvider &) = delete;
  MazdaSignalProvider &operator=(const MazdaSignalProvider &) = delete;
  MazdaSignalProvider(MazdaSignalProvider &&) = delete;
  MazdaSignalProvider &operator=(MazdaSignalProvider &&) = delete;

  // Non-owning view over the static catalog; valid for the program lifetime.
  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept;

  // Latest-state read of one catalog signal. InvalidSignal or
  // UnsupportedCapability are request failures, distinct from a successful
  // reading whose availability is NoData.
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalReading>
  read(vehicle_signals::SignalId id) const noexcept;

  // Stopped-only subscription to a Notify-capable catalog signal.
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalSubscription>
  subscribe(vehicle_signals::SignalId id, vehicle_signals::SignalCallback callback,
            void *context) noexcept;
  [[nodiscard]] vehicle_signals::SignalStatusResult
  unsubscribe(vehicle_signals::SignalSubscription subscription) noexcept;

private:
  VehicleTelemetry *telemetry_{nullptr};

  // Fixed opaque storage for the notification bridge's callback trampoline
  // records (one per typed notification subscriber slot). The implementation
  // owns it with placement construction and checks its record array fits, so
  // this header stays value-only and subscribing performs no heap allocation.
  static constexpr std::size_t kSubscriptionStorageBytes = 2048;
  alignas(std::max_align_t) std::byte subscription_storage_[kSubscriptionStorageBytes]{};
};

} // namespace mazda
