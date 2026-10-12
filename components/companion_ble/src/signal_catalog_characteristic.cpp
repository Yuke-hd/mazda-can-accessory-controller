#include "companion_ble/signal_catalog_characteristic.hpp"

#include "companion_protocol/signal_catalog_selector.hpp"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"

#include <array>
#include <cstdint>

namespace companion_ble::internal {
namespace {

using companion_protocol::SignalCatalogExport;

constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::size_t kSelectorBytes = companion_protocol::kSignalCatalogSelectorBytes;

// UUID 0006 is added to CompanionAttribute by the endpoint registration
// change. Keeping the numeric value here lets this independently owned
// characteristic compile before that registration slice is integrated.
constexpr CompanionAttribute kSignalCatalogAttribute = static_cast<CompanionAttribute>(0x0006U);

struct CatalogState {
  companion_protocol::SignalCatalogSelector selector{};
  std::uint16_t connection{kNoConnection};
  std::uint32_t next_token{0U};
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
  state.selector.connect(connection, next_connection_token());
}

void close_connection(const std::uint16_t connection) noexcept {
  if (connection != state.connection)
    return;
  state.selector.disconnect(connection);
  state.connection = kNoConnection;
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

int unavailable() noexcept { return static_cast<int>(companion_protocol::AttError::InvalidState); }

bool read_selector(const ble_gatt_access_ctxt &context,
                   std::array<std::uint8_t, kSelectorBytes> &selector) noexcept {
  if (OS_MBUF_PKTLEN(context.om) != kSelectorBytes)
    return false;
  std::uint16_t copied = 0U;
  return ble_hs_mbuf_to_flat(context.om, selector.data(), kSelectorBytes, &copied) == 0 &&
         copied == kSelectorBytes;
}

int access_signal_catalog(const std::uint16_t connection, std::uint16_t /*attribute*/,
                          ble_gatt_access_ctxt *const context, void * /*argument*/) {
  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    const auto page = state.selector.read(connection);
    return page.has_value() ? append(*context, page->view()) : unavailable();
  }
  if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;

  std::array<std::uint8_t, kSelectorBytes> selector{};
  if (!read_selector(*context, selector))
    return static_cast<int>(companion_protocol::AttError::InvalidAttributeValueLength);

  return static_cast<int>(state.selector.select(connection, selector));
}

} // namespace

CharacteristicDefinition signal_catalog_characteristic() noexcept {
  CharacteristicDefinition definition{kSignalCatalogAttribute};
  definition.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE;
  definition.access = access_signal_catalog;
  return definition;
}

int bind_signal_catalog(const SignalCatalogExport &catalog) noexcept {
  state.selector.bind(catalog);
  state.connection = kNoConnection;
  return ble_gap_event_listener_register(&state.listener, handle_gap_event, nullptr);
}

std::uint32_t signal_catalog_token_for_connection(const std::uint16_t connection) noexcept {
  return state.selector.token_for_connection(connection);
}

} // namespace companion_ble::internal
