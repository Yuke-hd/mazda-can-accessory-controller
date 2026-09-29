#pragma once

#include "controller_config/persisted/model.hpp"

namespace controller_config::persisted {

// The current production lighting profile (controller_config::
// kDefaultLightingProfile) as a version 1 persisted configuration: mirrored
// left/right turns, hazard, the RPM level fill and the RPM red zone. It is the
// C++ form of the example in docs/development/controller-config.md.
[[nodiscard]] ControllerConfig production_lighting_config();

} // namespace controller_config::persisted
