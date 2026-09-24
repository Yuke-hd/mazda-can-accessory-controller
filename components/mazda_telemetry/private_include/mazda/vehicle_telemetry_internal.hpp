#pragma once

#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

namespace mazda {
class MazdaSignalProvider;
}

namespace mazda::internal {

// Explicit assembly-only access for a firmware effect sink. This adapter is
// kept out of the public facade so ordinary consumers remain value-only and
// cannot acquire service or renderer ownership through transitive includes.
class VehicleTelemetryAccess final {
public:
  [[nodiscard]] static StatusResult bind_lighting_sink(VehicleTelemetry &facade,
                                                       LightingSink &sink) noexcept;
  [[nodiscard]] static VehicleTelemetryService &service(VehicleTelemetry &facade) noexcept;

#if !defined(ESP_PLATFORM)
  // Host-only test seam: bind the same public provider adapter directly to a
  // service constructed with a fake clock and injected acquisition source.
  // It does not add a public raw-frame or source-injection dependency.
  [[nodiscard]] static MazdaSignalProvider for_host_test(VehicleTelemetryService &service) noexcept;
#endif
};

} // namespace mazda::internal
