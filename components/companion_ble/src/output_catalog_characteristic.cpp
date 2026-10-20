#include "companion_ble/output_catalog_characteristic.hpp"

#include "companion_protocol/output_catalog_selector.hpp"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"

#include <array>
#include <cstdint>

namespace companion_ble::internal {
namespace {

using companion_protocol::OutputCatalogExport;

constexpr std::size_t kSelectorBytes = companion_protocol::kOutputCatalogSelectorBytes;
companion_protocol::OutputCatalogSelector selector_state{};

int append(ble_gatt_access_ctxt &context, const companion_protocol::ByteView bytes) noexcept {
  const int rc = os_mbuf_append(context.om, bytes.data(), static_cast<std::uint16_t>(bytes.size()));
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int unavailable() noexcept { return static_cast<int>(companion_protocol::AttError::InvalidState); }

bool read_selector(const ble_gatt_access_ctxt &context,
                   std::array<std::uint8_t, kSelectorBytes> &selector) noexcept {
  if (OS_MBUF_PKTLEN(context.om) != kSelectorBytes)
    return false;
  std::uint16_t copied = 0U;
  return ble_hs_mbuf_to_flat(context.om, selector.data(), kSelectorBytes, &copied) == 0 &&
         copied == kSelectorBytes;
}

int access_output_catalog(const std::uint16_t connection, std::uint16_t /*attribute*/,
                          ble_gatt_access_ctxt *const context, void * /*argument*/) {
  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    // Return the whole selected page on every invocation; NimBLE applies
    // Read Blob offsets. Reads never advance the document selector.
    const auto page = selector_state.read(connection);
    return page.has_value() ? append(*context, page->view()) : unavailable();
  }
  if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;

  std::array<std::uint8_t, kSelectorBytes> selector{};
  if (!read_selector(*context, selector))
    return static_cast<int>(companion_protocol::AttError::InvalidAttributeValueLength);

  return static_cast<int>(selector_state.select(connection, selector));
}

} // namespace

CharacteristicDefinition output_catalog_characteristic() noexcept {
  CharacteristicDefinition definition{CompanionAttribute::OutputCatalog};
  definition.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE;
  definition.access = access_output_catalog;
  return definition;
}

int bind_output_catalog(const OutputCatalogExport &catalog) noexcept {
  selector_state.bind(catalog);
  return 0;
}

void connect_output_catalog(const std::uint16_t connection, const std::uint32_t token) noexcept {
  selector_state.connect(connection, token);
}

void disconnect_output_catalog(const std::uint16_t connection) noexcept {
  selector_state.disconnect(connection);
}

} // namespace companion_ble::internal
