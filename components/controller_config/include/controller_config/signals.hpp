#pragma once

#include <string_view>

namespace controller_config {

// Engine speed in rpm. The RPM level fill and the RPM threshold both sample
// it; the provider reports it Read-only.
inline constexpr std::string_view kEngineRpmSignal{"vehicle.engine_rpm"};

} // namespace controller_config
