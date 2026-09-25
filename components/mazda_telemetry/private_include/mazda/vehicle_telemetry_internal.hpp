#pragma once

#include "mazda/vehicle_telemetry.hpp"
#include "mazda/vehicle_telemetry_service.hpp"

namespace mazda::internal {

// Explicit assembly-only access for a firmware effect sink. This adapter is
// kept out of the public facade so ordinary consumers remain value-only and
// cannot acquire service or renderer ownership through transitive includes.
class VehicleTelemetryAccess final {
public:
  [[nodiscard]] static StatusResult bind_lighting_sink(VehicleTelemetry &facade,
                                                       LightingSink &sink) noexcept;
  [[nodiscard]] static VehicleTelemetryService &service(VehicleTelemetry &facade) noexcept;
  [[nodiscard]] static const VehicleTelemetryService &
  service(const VehicleTelemetry &facade) noexcept;
#if !defined(ESP_PLATFORM)
  // Host-only test seam: replace a stopped facade's private service with one
  // bound to an injected clock, acquisition source and lighting sink, so host
  // tests exercise the production runtime, decoder and publication path.
  static void emplace_host_service(VehicleTelemetry &facade, vehicle_core::MonotonicClock &clock,
                                   vehicle_telemetry::AcquisitionSource &source,
                                   LightingSink &lighting_sink,
                                   const TelemetryConfig &config = {}) noexcept;
#endif
};

} // namespace mazda::internal
