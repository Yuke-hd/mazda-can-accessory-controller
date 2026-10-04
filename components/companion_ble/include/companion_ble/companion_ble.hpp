#pragma once

// Companion BLE service (docs/specs/companion/ble-protocol.md).
//
// The controller is a BLE peripheral and GATT server for the companion app.
// This component owns NimBLE initialization, advertising and the companion
// GATT service. It has no CAN, telemetry or LED dependency: a BLE failure
// leaves the controller without a companion link and never affects lighting.

#include "companion_protocol/device_info.hpp"

namespace companion_ble {

// Starts the companion BLE service on its own low-priority startup task and
// returns without waiting for the stack, so the caller's stack and timing are
// unaffected. Call it once, after startup black, NVS initialization and CAN
// start.
//
// `device_info` is encoded once here; its strings are not retained. The
// pairing window flag is always clear until pairing exists (#164).
//
// Returns false when start() already ran, a Device info string is longer than
// the protocol allows, or the startup task could not be created. Stack, GATT
// and advertising failures after that are logged by the component and leave
// the controller without a companion link.
[[nodiscard]] bool start(const companion_protocol::DeviceInfo &device_info) noexcept;

} // namespace companion_ble
