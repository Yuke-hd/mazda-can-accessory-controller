#pragma once

#include "mazda/vehicle_telemetry.hpp"

namespace mazda::application {

// Bind the firmware's private local ARGB sink to a public telemetry facade.
// The returned facade remains the only application-facing telemetry API; the
// renderer and its queue stay behind this explicit assembly boundary.
[[nodiscard]] StatusResult bind_local_argb_sink(VehicleTelemetry &telemetry) noexcept;

} // namespace mazda::application
