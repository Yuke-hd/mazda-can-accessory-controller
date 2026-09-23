#pragma once

#include "vehicle_core/reading.hpp"

namespace mazda {

// The Mazda facade uses the same value-copy reading contract as the portable
// vehicle core. Keep the aliases here so consumers of this focused header do
// not need to include the availability evaluator just to name a reading.
using vehicle_core::Availability;
using vehicle_core::Reading;
using vehicle_core::ValidationStatus;

} // namespace mazda
