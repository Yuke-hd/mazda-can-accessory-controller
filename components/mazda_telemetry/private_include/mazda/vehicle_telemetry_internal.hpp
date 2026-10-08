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
  // Host composition seam: replace a stopped facade's private service with
  // one bound to an injected clock and acquisition source. Deterministic
  // replay may additionally select manual notification dispatch and a
  // post-publication worker control without exposing either through the
  // value-only public facade.
  static void emplace_host_service(VehicleTelemetry &facade, vehicle_core::MonotonicClock &clock,
                                   vehicle_telemetry::AcquisitionSource &source,
                                   LightingSink &lighting_sink, const TelemetryConfig &config = {},
                                   HostServiceOptions host_options = {}) noexcept;
  [[nodiscard]] static Result<std::size_t>
  drain_host_notifications(VehicleTelemetry &facade) noexcept;
#else
  // Benchmark-only composition seam. The firmware synthetic workload uses
  // the real Runtime worker with a fixed AcquisitionSource while keeping the
  // production facade's CanBusSource private and untouched.
  static void emplace_benchmark_service(VehicleTelemetry &facade,
                                        vehicle_core::MonotonicClock &clock,
                                        vehicle_telemetry::AcquisitionSource &source,
                                        LightingSink &lighting_sink,
                                        const TelemetryConfig &config = {}) noexcept;
#endif
};

} // namespace mazda::internal
