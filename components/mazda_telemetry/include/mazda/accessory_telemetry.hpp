#pragma once

#include "mazda/vehicle_telemetry.hpp"

namespace mazda::application {

// Bind the firmware's private local ARGB sink to a public telemetry facade.
// The returned facade remains the only application-facing telemetry API; the
// renderer and its queue stay behind this explicit assembly boundary.
//
// Rollout fallback only: the WeAct firmware now drives the strip through the
// generic action engine and no longer binds this sink. The renderer queue has
// a single publisher, so never bind this alongside the engine's LED sink.
[[nodiscard]] StatusResult bind_local_argb_sink(VehicleTelemetry &telemetry) noexcept;

} // namespace mazda::application
