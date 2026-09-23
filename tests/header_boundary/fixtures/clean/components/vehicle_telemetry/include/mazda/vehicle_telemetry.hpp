#pragma once

#include "mazda/facade_contracts.hpp"

namespace mazda {

class VehicleTelemetry final {
public:
  VehicleTelemetry() noexcept = default;
  [[nodiscard]] StatusResult start() noexcept { return {}; }
};

} // namespace mazda
