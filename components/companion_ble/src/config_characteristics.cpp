#include "companion_ble/config_characteristics.hpp"

#include "companion_ble/security.hpp"
#include "companion_protocol/command.hpp"
#include "companion_protocol/config_status.hpp"
#include "companion_protocol/config_transfer.hpp"
#include "companion_protocol/read_back.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include <array>
#include <cstdint>
#include <optional>

namespace companion_ble::internal {
namespace {

using companion_protocol::AttError;
using companion_protocol::ByteView;
using companion_protocol::CommandOpcode;
using companion_protocol::ConfigBoot;
using companion_protocol::ConfigTransfer;
using companion_protocol::TransferClock;

constexpr const char *kTag = "companion_ble";
// ATT limits an attribute value to 512 bytes; every valid PDU is shorter.
constexpr std::uint16_t kMaxWriteBytes = 512U;
// The disconnect and restart sequence waits at most this long for the
// disconnection before it restarts anyway (ble-protocol.md).
constexpr std::uint32_t kRestartDisconnectWaitMs = 1000U;

// All transfer state. Written once by prepare_config_transfer() on the
// startup task before the host task exists, then touched only on the host
// task.
struct ConfigState {
  ConfigBoot boot{};
  companion_protocol::FactoryReverter *reverter{nullptr};
  std::uint16_t status_handle{0U};
  bool status_subscribed{false};
  // Select read page offset of the current connection; 0 on each connect.
  std::uint16_t page_offset{0U};
  ble_npl_callout idle_timer{};
  ble_npl_callout restart_timer{};
  ble_gap_event_listener listener{};
};

// GCC emits a class with default member initializers to .data even when
// every byte is zero, so the large buffers below are separate statics and land
// in .bss.
ConfigState state{};
std::array<std::uint8_t, kMaxWriteBytes> write_buffer{};
// About 4.2 KiB with its upload buffer, so it is static, not on a stack.
std::optional<ConfigTransfer> transfer{};
// The controller accepts one connection at a time (ble-protocol.md). Kept out
// of ConfigState because its nonzero initializer would move the state to .data.
std::uint16_t current_connection{BLE_HS_CONN_HANDLE_NONE};

TransferClock now() noexcept { return TransferClock{esp_timer_get_time() / 1000}; }

void arm_ms(ble_npl_callout &timer, const std::uint32_t milliseconds) noexcept {
  // One extra tick so a timer never fires before its deadline.
  (void)ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(milliseconds) + 1U);
}

// Notifies Config status to the subscribed link, but only at the transfer MTU
// and only on a link with an accepted bond (ble-protocol.md, "Notification
// gate"). ble_gatts_notify_custom() ignores the CCCD, so the subscription is
// tracked from GAP subscribe events.
void notify_status() noexcept {
  const std::uint16_t connection = current_connection;
  if (connection == BLE_HS_CONN_HANDLE_NONE || !state.status_subscribed ||
      ble_att_mtu(connection) < companion_protocol::kMinimumTransferMtu ||
      !link_has_accepted_bond(connection))
    return;
  const auto value =
      companion_protocol::encode_config_status(state.boot.status, transfer->status());
  const ByteView bytes = value.view();
  os_mbuf *const om = ble_hs_mbuf_from_flat(bytes.data(), static_cast<std::uint16_t>(bytes.size()));
  if (om == nullptr)
    return;
  // Consumes `om` whatever the result.
  const int rc = ble_gatts_notify_custom(connection, state.status_handle, om);
  if (rc != 0)
    ESP_LOGW(kTag, "Config status notification failed (rc=%d)", rc);
}

// Keeps the idle timer running while a transfer is open. The transfer itself
// decides expiry; the timer only makes sure it is asked.
void track_idle_timeout() noexcept {
  if (transfer->status().state == companion_protocol::TransferState::Receiving)
    arm_ms(state.idle_timer,
           static_cast<std::uint32_t>(companion_protocol::kTransferIdleTimeout.count()));
  else
    ble_npl_callout_stop(&state.idle_timer);
}

// The disconnect and restart sequence after a Saved commit or an accepted
// Revert to factory. The terminate is queued behind the write response; the
// restart follows the disconnect, or the wait limit.
void schedule_restart(const std::uint16_t connection) noexcept {
  ESP_LOGI(kTag, "config change accepted; restarting after disconnect");
  disconnect_after_response(connection);
  arm_ms(state.restart_timer, kRestartDisconnectWaitMs);
}

void on_idle_timer(ble_npl_event * /*event*/) {
  if (transfer->expire_if_idle(now()))
    notify_status();
  track_idle_timeout();
}

void on_restart_timer(ble_npl_event * /*event*/) {
  // Normal boot path: startup black, then the selected config.
  esp_restart();
}

int on_gap_event(ble_gap_event *const event, void * /*argument*/) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      current_connection = event->connect.conn_handle;
      state.status_subscribed = false;
      state.page_offset = 0U;
    }
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    if (event->disconnect.conn.conn_handle == current_connection) {
      current_connection = BLE_HS_CONN_HANDLE_NONE;
      state.status_subscribed = false;
      state.page_offset = 0U;
      (void)transfer->interrupt();
      track_idle_timeout();
      if (transfer->restart_pending())
        arm_ms(state.restart_timer, 0U);
    }
    break;
  case BLE_GAP_EVENT_SUBSCRIBE:
    if (event->subscribe.conn_handle == current_connection &&
        event->subscribe.attr_handle == state.status_handle)
      state.status_subscribed = event->subscribe.cur_notify != 0U;
    break;
  default:
    break;
  }
  return 0;
}

// Copies the written value into the static write buffer. Returns false when
// it does not fit.
bool read_write_value(const ble_gatt_access_ctxt &context, ByteView &value) noexcept {
  const std::uint16_t length = OS_MBUF_PKTLEN(context.om);
  if (length > write_buffer.size())
    return false;
  std::uint16_t copied = 0U;
  if (ble_hs_mbuf_to_flat(context.om, write_buffer.data(), length, &copied) != 0)
    return false;
  value = ByteView{write_buffer.data(), copied};
  return true;
}

int append(ble_gatt_access_ctxt &context, const ByteView bytes) noexcept {
  // NimBLE applies Read Blob offsets itself, so the whole value is appended.
  const int rc = os_mbuf_append(context.om, bytes.data(), static_cast<std::uint16_t>(bytes.size()));
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int access_config(const std::uint16_t connection, std::uint16_t /*attribute*/,
                  ble_gatt_access_ctxt *const context, void * /*argument*/) {
  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    const auto page =
        companion_protocol::encode_read_back_page(state.boot.status.active, state.page_offset);
    return page.has_value() ? append(*context, page->view()) : BLE_ATT_ERR_UNLIKELY;
  }
  if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;

  ByteView pdu{};
  if (!read_write_value(*context, pdu))
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  // A commit, with its parse, dry run and NVS write, runs here, inside the
  // write handler on the host task (config-transfer.md, "Commit").
  const auto response = transfer->handle_write(pdu, {ble_att_mtu(connection), now()});
  if (response.read_page_offset.has_value())
    state.page_offset = *response.read_page_offset;
  if (response.status_changed) {
    // The host task stack size is not yet measured on hardware
    // (ble-resource-budget.md); report its headroom after each state change,
    // which includes every commit.
    ESP_LOGI(kTag, "config transfer state changed; host stack headroom=%u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    notify_status();
  }
  track_idle_timeout();
  if (transfer->restart_pending())
    schedule_restart(connection);
  return static_cast<int>(response.error);
}

int access_config_status(std::uint16_t /*connection*/, std::uint16_t /*attribute*/,
                         ble_gatt_access_ctxt *const context, void * /*argument*/) {
  if (context->op != BLE_GATT_ACCESS_OP_READ_CHR)
    return BLE_ATT_ERR_UNLIKELY;
  const auto value =
      companion_protocol::encode_config_status(state.boot.status, transfer->status());
  return append(*context, value.view());
}

AttError revert_to_factory(const std::uint16_t connection) noexcept {
  if (state.reverter == nullptr || !state.reverter->revert_to_factory())
    return AttError::StorageFailure;
  // check_busy() just found no open transfer, so this cannot fail.
  if (!transfer->enter_restart_pending())
    return AttError::Busy;
  notify_status();
  schedule_restart(connection);
  return AttError::None;
}

AttError clear_bonds_command(const std::uint16_t connection) noexcept {
  if (!clear_bonds())
    return AttError::StorageFailure;
  disconnect_after_response(connection);
  return AttError::None;
}

int access_command(const std::uint16_t connection, std::uint16_t /*attribute*/,
                   ble_gatt_access_ctxt *const context, void * /*argument*/) {
  if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_UNLIKELY;
  ByteView pdu{};
  if (!read_write_value(*context, pdu))
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  const auto busy = transfer->check_busy(now());
  if (busy.status_changed) {
    notify_status();
    track_idle_timeout();
  }
  const auto decision = companion_protocol::decode_command(pdu, busy.busy);
  if (decision.error != AttError::None || !decision.command.has_value())
    return static_cast<int>(decision.error);
  switch (*decision.command) {
  case CommandOpcode::RevertToFactory:
    return static_cast<int>(revert_to_factory(connection));
  case CommandOpcode::ClearBonds:
    return static_cast<int>(clear_bonds_command(connection));
  }
  return static_cast<int>(AttError::UnsupportedOperation);
}

} // namespace

bool prepare_config_transfer(const ConfigInputs &inputs) noexcept {
  if (inputs.boot == nullptr || inputs.committer == nullptr || inputs.reverter == nullptr)
    return false;
  // Serializes the active config once; the ActiveDocument it returns views
  // bytes owned by the boot source until restart, and both Config status and
  // read-back use this one value.
  state.boot = inputs.boot->prepare_boot();
  state.reverter = inputs.reverter;
  transfer.emplace(*inputs.committer, state.boot.environment());
  return true;
}

void configure_config_transfer() noexcept {
  ble_npl_eventq *const queue = nimble_port_get_dflt_eventq();
  ble_npl_callout_init(&state.idle_timer, queue, on_idle_timer, nullptr);
  ble_npl_callout_init(&state.restart_timer, queue, on_restart_timer, nullptr);
  const int rc = ble_gap_event_listener_register(&state.listener, on_gap_event, nullptr);
  if (rc != 0)
    ESP_LOGW(kTag, "config transfer GAP listener failed (rc=%d)", rc);
}

CharacteristicDefinition config_characteristic() noexcept {
  CharacteristicDefinition definition{CompanionAttribute::Config};
  definition.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE;
  definition.access = access_config;
  return definition;
}

CharacteristicDefinition config_status_characteristic() noexcept {
  CharacteristicDefinition definition{CompanionAttribute::ConfigStatus};
  definition.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
  definition.access = access_config_status;
  definition.value_handle = &state.status_handle;
  return definition;
}

CharacteristicDefinition command_characteristic() noexcept {
  CharacteristicDefinition definition{CompanionAttribute::Command};
  definition.flags = BLE_GATT_CHR_F_WRITE;
  definition.access = access_command;
  return definition;
}

} // namespace companion_ble::internal
