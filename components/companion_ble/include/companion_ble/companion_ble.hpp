#pragma once

// Companion BLE service (docs/specs/companion/ble-protocol.md).
//
// The controller is a BLE peripheral and GATT server for the companion app.
// This component owns NimBLE initialization, advertising, the companion GATT
// service and its pairing policy (ADR-0003). It has no CAN, telemetry, LED,
// GPIO or NVS dependency: the composition root hands it the user key sampler
// and the NVS initialization result. A BLE failure leaves the controller
// without a companion link and never affects lighting.

#include "companion_protocol/device_info.hpp"
#include "companion_protocol/pairing_window.hpp"

namespace companion_ble {

// Samples the user key once; true while it is pressed. Called every 20 ms on
// the NimBLE host task, so it must not block.
using UserKeySampler = bool (*)();

struct PairingInputs {
  // Unavailable when NVS initialization failed this boot: every pairing is
  // rejected and the pairing window never opens.
  companion_protocol::BondStorage bond_storage{companion_protocol::BondStorage::Unavailable};
  // Null disables the pairing window.
  UserKeySampler user_key_pressed{nullptr};
};

// Starts the companion BLE service on its own low-priority startup task and
// returns without waiting for the stack, so the caller's stack and timing are
// unaffected. Call it once, after startup black, NVS initialization and CAN
// start.
//
// `device_info` is encoded once here; its strings are not retained. The
// served pairing window flag reflects the live window state, whatever
// `device_info.pairing_window_open` says.
//
// Returns false when start() already ran, a Device info string is longer than
// the protocol allows, or the startup task could not be created. Stack, GATT
// and advertising failures after that are logged by the component and leave
// the controller without a companion link.
[[nodiscard]] bool start(const companion_protocol::DeviceInfo &device_info,
                         const PairingInputs &pairing) noexcept;

} // namespace companion_ble
