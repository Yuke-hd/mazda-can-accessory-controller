#include "mazda/signal_provider.hpp"

#include "mazda/signal_catalog.hpp"

namespace mazda {

// Construction and the static catalog live here. read() belongs to the
// generic read adapter and subscribe()/unsubscribe() to the notification
// bridge; each is defined in its own source file.

MazdaSignalProvider::MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept
    : telemetry_(&telemetry) {}

vehicle_signals::SignalCatalogView MazdaSignalProvider::catalog() const noexcept {
  return internal::signal_catalog();
}

} // namespace mazda
