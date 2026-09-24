#pragma once

#include <cstddef>
#include <cstdint>

#include "vehicle_signals/catalog.hpp"
#include "vehicle_signals/types.hpp"

namespace mazda::internal {
class VehicleTelemetryAccess;
class VehicleTelemetryService;
} // namespace mazda::internal

namespace mazda {

class VehicleTelemetry;

// Provider-neutral catalog/read/notification surface for the Mazda facade.
// VehicleTelemetry is forward-declared so this header does not expose its
// implementation or internal service types. Callback contexts remain owned by
// the caller and must live through a successful stop that quiesces callbacks.
// Keep the provider alive while its backing telemetry can invoke callbacks.
// Destroy it on the lifecycle owner after a successful stop and while the
// backing telemetry remains alive; destruction removes active registrations
// without implicitly stopping the service. If stop fails or times out, retain
// the provider and callback contexts until a later stop succeeds.
class MazdaSignalProvider final {
public:
  explicit MazdaSignalProvider(VehicleTelemetry &telemetry) noexcept;
  ~MazdaSignalProvider() noexcept;

  MazdaSignalProvider(const MazdaSignalProvider &) = delete;
  MazdaSignalProvider &operator=(const MazdaSignalProvider &) = delete;
  MazdaSignalProvider(MazdaSignalProvider &&) = delete;
  MazdaSignalProvider &operator=(MazdaSignalProvider &&) = delete;

  [[nodiscard]] vehicle_signals::SignalCatalogView catalog() const noexcept;
  [[nodiscard]] vehicle_signals::SignalReadResult
  read(vehicle_signals::SignalId signal) const noexcept;
  [[nodiscard]] vehicle_signals::SignalSubscriptionResult
  subscribe(vehicle_signals::SignalId signal, vehicle_signals::SignalCallback callback,
            void *context) noexcept;
  [[nodiscard]] vehicle_signals::SignalStatus
  unsubscribe(vehicle_signals::SignalSubscriptionToken token) noexcept;

private:
  friend class internal::VehicleTelemetryAccess;

  class Impl;
  explicit MazdaSignalProvider(internal::VehicleTelemetryService &service) noexcept;

  static constexpr std::size_t kImplementationStorageBytes = 4096;
  VehicleTelemetry *telemetry_{nullptr};
  internal::VehicleTelemetryService *service_{nullptr};
  alignas(std::max_align_t) std::byte implementation_storage_[kImplementationStorageBytes]{};
};

} // namespace mazda
