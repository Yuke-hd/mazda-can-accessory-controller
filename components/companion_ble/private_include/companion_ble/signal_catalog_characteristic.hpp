#pragma once

#include "companion_ble/gatt_service.hpp"
#include "companion_protocol/signal_catalog_export.hpp"

namespace companion_ble::internal {

// The protected Read/Write-with-response signal catalog characteristic. The
// endpoint is defined before the catalog is bound so service registration can
// keep the characteristic table static.
[[nodiscard]] CharacteristicDefinition signal_catalog_characteristic() noexcept;

// Binds the immutable export used by the catalog pages and installs the GAP
// listener that creates and clears per-connection selector state. The export
// must outlive the NimBLE host. This function does not read or subscribe to a
// provider; the export has already resolved the provider catalog.
[[nodiscard]] int
bind_signal_catalog(const companion_protocol::SignalCatalogExport &catalog) noexcept;

} // namespace companion_ble::internal
