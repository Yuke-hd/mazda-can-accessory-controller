#pragma once

#include "companion_ble/gatt_service.hpp"
#include "companion_protocol/output_catalog_export.hpp"

#include <cstdint>

namespace companion_ble::internal {

// The protected Read/Write-with-response output catalog characteristic. The
// endpoint is defined before the catalog is bound so service registration can
// keep the characteristic table static.
[[nodiscard]] CharacteristicDefinition output_catalog_characteristic() noexcept;

// Binds the immutable export used by the catalog pages. The export
// must outlive the NimBLE host. It holds only the opaque document view; no
// descriptor parsing or provider access occurs in this component.
[[nodiscard]] int
bind_output_catalog(const companion_protocol::OutputCatalogExport &catalog) noexcept;

void connect_output_catalog(std::uint16_t connection, std::uint32_t token) noexcept;
void disconnect_output_catalog(std::uint16_t connection) noexcept;

} // namespace companion_ble::internal
