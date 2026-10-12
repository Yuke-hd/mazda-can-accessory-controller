#pragma once

#include "companion_ble/gatt_service.hpp"
#include "companion_protocol/signal_catalog_export.hpp"

#include <cstdint>

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

// Returns the discovery token for the active connection, or zero for any
// other connection. Later catalog-backed notifications use the same token so
// a client cannot mix a page stream with a different connection's catalog.
[[nodiscard]] std::uint32_t signal_catalog_token_for_connection(std::uint16_t connection) noexcept;

} // namespace companion_ble::internal
