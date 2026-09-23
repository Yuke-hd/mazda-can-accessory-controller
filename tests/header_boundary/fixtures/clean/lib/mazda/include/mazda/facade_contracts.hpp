#pragma once

#include "vehicle_core/telemetry_contracts.hpp"

namespace mazda {

struct TelemetryConfig {};
struct StatusResult {
  constexpr explicit operator bool() const noexcept { return true; }
};

} // namespace mazda
