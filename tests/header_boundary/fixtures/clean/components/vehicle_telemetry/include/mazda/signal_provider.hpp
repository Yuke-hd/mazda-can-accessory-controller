#pragma once

#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace mazda {

class VehicleTelemetry;

class MazdaSignalProvider final {
public:
  explicit MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept : telemetry_(&telemetry) {}

  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept { return {}; }
  [[nodiscard]] vehicle_signals::SignalResult<vehicle_signals::SignalReading>
  read(vehicle_signals::SignalId) const noexcept {
    return vehicle_signals::SignalResult<vehicle_signals::SignalReading>::failure(
        vehicle_signals::SignalStatus::InvalidSignal);
  }

private:
  VehicleTelemetry *telemetry_{nullptr};
};

} // namespace mazda
