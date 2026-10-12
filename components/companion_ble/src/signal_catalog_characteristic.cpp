#include "companion_ble/signal_catalog_characteristic.hpp"

#include "companion_protocol/att_error.hpp"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"

#include <array>
#include <cstdint>

namespace companion_ble::internal {
namespace {

using companion_protocol::AttError;
using companion_protocol::SignalCatalogExport;

constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::uint8_t kCatalogFormat = companion_protocol::kSignalCatalogFormat;
constexpr std::size_t kSelectorBytes = 7U;

// UUID 0006 is added to CompanionAttribute by the endpoint registration
// change. Keeping the numeric value here lets this independently owned
// characteristic compile before that registration slice is integrated.
constexpr CompanionAttribute kSignalCatalogAttribute = static_cast<CompanionAttribute>(0x0006U);

struct CatalogState {
  const SignalCatalogExport *catalog{nullptr};
  std::uint16_t connection{kNoConnection};
  std::uint32_t token{0U};
  std::uint32_t next_token{0U};
  std::uint16_t page_offset{0U};
  ble_gap_event_listener listener{};
};

CatalogState state{};

std::uint32_t next_connection_token() noexcept {
  ++state.next_token;
  if (state.next_token == 0U)
    ++state.next_token;
  return state.next_token;
}

void open_connection(const std::uint16_t connection) noexcept {
  // The current firmware accepts one connection at a time. An unexpected
  // second CONNECT must not take over the first link's selector state.
  if (state.connection != kNoConnection)
    return;
  state.connection = connection;
  state.token = next_connection_token();
  state.page_offset = 0U;
}

void close_connection(const std::uint16_t connection) noexcept {
  if (connection != state.connection)
    return;
  state.connection = kNoConnection;
  state.token = 0U;
  state.page_offset = 0U;
}

int handle_gap_event(ble_gap_event *const event, void * /*argument*/) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0)
      open_connection(event->connect.conn_handle);
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    close_connection(event->disconnect.conn.conn_handle);
    break;
  default:
    break;
  }
  return 0;
}

int append(ble_gatt_access_ctxt &context, const companion_protocol::ByteView bytes) noexcept {
  const int rc = os_mbuf_append(context.om, bytes.data(), static_cast<std::uint16_t>(bytes.size()));
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int unavailable() noexcept { return static_cast<int>(AttError::InvalidState); }

bool read_selector(const ble_gatt_access_ctxt &context,
                   std::array<std::uint8_t, kSelectorBytes> &selector) noexcept {
  if (OS_MBUF_PKTLEN(context.om) != kSelectorBytes)
    return false;
  std::uint16_t copied = 0U;
  return ble_hs_mbuf_to_flat(context.om, selector.data(), kSelectorBytes, &copied) == 0 &&
         copied == kSelectorBytes;
}

std::uint32_t read_u32(const std::array<std::uint8_t, kSelectorBytes> &selector) noexcept {
  return static_cast<std::uint32_t>(selector[1]) | (static_cast<std::uint32_t>(selector[2]) << 8U) |
         (static_cast<std::uint32_t>(selector[3]) << 16U) |
         (static_cast<std::uint32_t>(selector[4]) << 24U);
}

std::uint16_t read_u16(const std::array<std::uint8_t, kSelectorBytes> &selector) noexcept {
  return static_cast<std::uint16_t>(selector[5]) | (static_cast<std::uint16_t>(selector[6]) << 8U);
}

int access_signal_catalog(const std::uint16_t connection, std::uint16_t /*attribute*/,
                          ble_gatt_access_ctxt *const context, void * /*argument*/) {
  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (connection != state.connection || state.catalog == nullptr || state.token == 0U ||
        !state.catalog->available())
      return unavailable();
    const auto page = state.catalog->encode_page(state.token, state.page_offset);
    return page.has_value() ? append(*context, page->view()) : unavailable();
  }
  if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;

  std::array<std::uint8_t, kSelectorBytes> selector{};
  if (!read_selector(*context, selector))
    return static_cast<int>(AttError::InvalidAttributeValueLength);

  // Selector validation order is part of the catalog wire contract. Do not
  // inspect export state or the offset before rejecting an unknown format.
  if (selector[0] != kCatalogFormat)
    return static_cast<int>(AttError::UnsupportedOperation);
  if (connection != state.connection || state.catalog == nullptr || state.token == 0U ||
      !state.catalog->available() || read_u32(selector) != state.token)
    return unavailable();

  const std::uint16_t offset = read_u16(selector);
  if (offset > state.catalog->document_length())
    return static_cast<int>(AttError::InvalidPdu);

  // Commit only after all validation succeeds, so a failed write leaves the
  // currently selected page unchanged.
  state.page_offset = offset;
  return static_cast<int>(AttError::None);
}

} // namespace

CharacteristicDefinition signal_catalog_characteristic() noexcept {
  CharacteristicDefinition definition{kSignalCatalogAttribute};
  definition.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE;
  definition.access = access_signal_catalog;
  return definition;
}

int bind_signal_catalog(const SignalCatalogExport &catalog) noexcept {
  state.catalog = &catalog;
  state.connection = kNoConnection;
  state.token = 0U;
  state.page_offset = 0U;
  return ble_gap_event_listener_register(&state.listener, handle_gap_event, nullptr);
}

} // namespace companion_ble::internal
