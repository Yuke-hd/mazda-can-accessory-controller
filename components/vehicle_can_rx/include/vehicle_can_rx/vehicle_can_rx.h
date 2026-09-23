#pragma once

namespace vehicle_can_rx {

// The vehicle binding is a build-selected application dependency. It has no
// runtime mode knob and intentionally exposes no frame-transmission operation.
inline constexpr char kTargetName[] = "weact_can485_v11_vehicle_listen_only";

} // namespace vehicle_can_rx
