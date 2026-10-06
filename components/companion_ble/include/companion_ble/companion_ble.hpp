#pragma once

// Companion BLE service (docs/specs/companion/ble-protocol.md).
//
// The controller is a BLE peripheral and GATT server for the companion app.
// This component owns NimBLE initialization, advertising, the companion GATT
// service and its pairing policy (ADR-0003). It has no CAN, telemetry, LED,
// GPIO or NVS dependency: the composition root hands it the user key sampler,
// the NVS initialization result, the config ports and the read-only live
// signal provider. A BLE failure leaves the controller without a companion
// link and never affects lighting.

#include "companion_protocol/config_ports.hpp"
#include "companion_protocol/config_transfer.hpp"
#include "companion_protocol/device_info.hpp"
#include "companion_protocol/pairing_window.hpp"
#include "vehicle_signals/signal_provider.hpp"

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

// Performs the controlled restart that ends a successful commit or Revert to
// factory. The composition root owns it: it fails the lighting off to black
// through the lighting layer, then restarts. It runs on the NimBLE host task
// and must not return.
using ControlledRestart = void (*)() noexcept;

// The composition root's config ports (config-transfer.md). They must outlive
// the NimBLE host. With every member set, start() serves Config, Config
// status and Command; otherwise only Device info. Every port call runs on the
// BLE tasks: prepare_boot() once on the startup task, commits, reverts and the
// restart on the NimBLE host task. The restart is called after the
// disconnection, or after the 1 s disconnect wait (ble-protocol.md,
// "Disconnect and restart sequence").
struct ConfigInputs {
  companion_protocol::ConfigBootSource *boot{nullptr};
  companion_protocol::ConfigCommitter *committer{nullptr};
  companion_protocol::FactoryReverter *reverter{nullptr};
  ControlledRestart restart{nullptr};
};

// Everything the composition root hands to start().
struct ServiceInputs {
  PairingInputs pairing{};
  ConfigInputs config{};
  // The read-only source of the Live signals characteristic
  // (docs/specs/companion/live-signals.md). Required. It is sampled with
  // SignalProvider::read() only, from the NimBLE host task, so it must
  // outlive the NimBLE host; the service never subscribes to it.
  const vehicle_signals::SignalProvider *live_signals{nullptr};
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
// Returns false when start() already ran, `inputs.live_signals` is null, a
// Device info string is longer than the protocol allows, or the startup task
// could not be created. Stack, GATT and advertising failures after that are
// logged by the component and leave the controller without a companion link.
[[nodiscard]] bool start(const companion_protocol::DeviceInfo &device_info,
                         const ServiceInputs &inputs) noexcept;

// Records that the telemetry facade started successfully during this boot, so
// live signal frames set flags bit 0 from then on. Safe from any task and
// before or after start(); it never fails and never waits.
void mark_telemetry_started() noexcept;

} // namespace companion_ble
