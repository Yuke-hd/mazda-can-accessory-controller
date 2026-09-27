#pragma once

#include "vehicle_core/time.hpp"

namespace controller_config {

// The application samples polled action rules at this cadence. Host replay
// uses the same value so a replay follows the firmware composition root.
inline constexpr vehicle_core::Microseconds kPolledRuleSamplePeriodUs{100'000};

} // namespace controller_config
