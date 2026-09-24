#include "mazda/signal_provider.hpp"

#include <new>

#include "mazda/signal_catalog.hpp"
#include "mazda/signal_subscription_bridge.hpp"

namespace mazda {

// Construction and the static catalog live here. read() belongs to the
// generic read adapter and subscribe()/unsubscribe() to the notification
// bridge; each is defined in its own source file.

MazdaSignalProvider::MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept
    : telemetry_(&telemetry) {
  static_assert(sizeof(internal::SignalSubscriptionBridge) <= sizeof(subscription_storage_));
  static_assert(alignof(internal::SignalSubscriptionBridge) <= alignof(std::max_align_t));
  ::new (static_cast<void *>(subscription_storage_)) internal::SignalSubscriptionBridge{};
}

vehicle_signals::SignalCatalogView MazdaSignalProvider::catalog() const noexcept {
  return internal::signal_catalog();
}

} // namespace mazda
