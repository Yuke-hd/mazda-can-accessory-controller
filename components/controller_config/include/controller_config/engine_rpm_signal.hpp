#pragma once

#include <string_view>

namespace controller_config {

// The engine speed signal the RPM features sample. The provider reports it
// Read-only and always FreshnessUnverified, so RPM rules accept
// FreshOrUnverified readings.
inline constexpr std::string_view kEngineRpmSignal{"vehicle.engine_rpm"};

} // namespace controller_config
