#pragma once

#include "companion_ble/gatt_service.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace companion_ble::internal {

// The notify-only Live signals characteristic
// (docs/specs/companion/live-signals.md). Its definition can be built before
// the stream is bound.
[[nodiscard]] CharacteristicDefinition live_signals_characteristic() noexcept;

// Binds the stream to the read-only `provider`, which must outlive the host,
// and hooks it to the NimBLE host: resolves the
// signal catalog once, creates the sampling timer on the host's default event
// queue and registers a GAP event listener. Sampling, pacing and every
// notification then run on the NimBLE host task, and only while a connected
// central has enabled notifications on a secured link with an ATT MTU of at
// least 64. Call it once, after nimble_port_init() and service registration
// and before the host task starts. Returns 0 or a NimBLE host error code.
[[nodiscard]] int bind_live_signals(const vehicle_signals::SignalProvider &provider) noexcept;

// Sets frame flags bit 0 from the next sample on. Safe from any task.
void mark_live_signals_telemetry_started() noexcept;

} // namespace companion_ble::internal
