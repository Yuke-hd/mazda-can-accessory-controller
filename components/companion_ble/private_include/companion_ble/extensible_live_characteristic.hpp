#pragma once

#include "companion_ble/gatt_service.hpp"
#include "companion_protocol/signal_catalog_export.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace companion_ble::internal {

// The notify-only extensible live-signals characteristic (UUID 0007). Its
// definition is created before the immutable catalog is bound so the GATT
// table can remain static.
[[nodiscard]] CharacteristicDefinition extensible_live_signals_characteristic() noexcept;

// Binds the catalog and read-only provider to a bounded NimBLE host-task
// callout and GAP listener. Both references must outlive the host. The
// catalog listener must be bound first so its per-connection token is
// available before this stream samples. Call once after nimble_port_init()
// and GATT registration, before the host task starts. Returns 0 or a NimBLE
// host error code.
[[nodiscard]] int
bind_extensible_live_signals(const companion_protocol::SignalCatalogExport &catalog,
                             const vehicle_signals::SignalProvider &provider) noexcept;

// Sets the packet telemetry-started flag from the next sample. Safe from any
// task; it never waits for the BLE host.
void mark_extensible_live_signals_telemetry_started() noexcept;

} // namespace companion_ble::internal
