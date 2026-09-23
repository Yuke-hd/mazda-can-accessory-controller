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
};

} // namespace mazda::internal
