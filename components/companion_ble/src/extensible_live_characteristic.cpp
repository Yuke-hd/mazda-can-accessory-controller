#include "companion_ble/extensible_live_characteristic.hpp"

#include "companion_ble/security.hpp"
#include "companion_ble/signal_catalog_characteristic.hpp"
#include "companion_protocol/extensible_live_stream.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "nimble/nimble_port.h"

#include <atomic>
#include <cstdint>
#include <optional>

// Except for bind_extensible_live_signals() and
// mark_extensible_live_signals_telemetry_started(), all state below is owned
// by the NimBLE host task. Its callout and GAP listener therefore serialize
// stream state with characteristic writes. Packet queueing never waits; an
// unavailable mbuf or a rejected notification drops this packet, and the
// portable stream retries current readings at a later slot.

namespace companion_ble::internal {
namespace {

using companion_protocol::ExtensibleLiveSignalPacket;
using companion_protocol::ExtensibleLiveSignalStream;
using companion_protocol::LiveStreamClock;
using companion_protocol::SignalCatalogExport;

constexpr const char *kTag = "companion_ble";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr CompanionAttribute kExtensibleLiveSignalsAttribute =
    static_cast<CompanionAttribute>(0x0007U);

class NotificationSink final : public companion_protocol::ExtensibleLiveFrameSink {
public:
  bool try_queue(const ExtensibleLiveSignalPacket &packet) noexcept override {
    if (connection == kNoConnection || value_handle == 0U || packet.size() == 0U) {
      ++dropped;
      return false;
    }

    // The protocol caps packets at 244 bytes. NimBLE owns the mbuf after the
    // call, including the failure path, and the host context does not wait for
    // pool capacity.
    os_mbuf *const payload =
        ble_hs_mbuf_from_flat(packet.view().data(), static_cast<std::uint16_t>(packet.size()));
    const bool queued =
        payload != nullptr && ble_gatts_notify_custom(connection, value_handle, payload) == 0;
    ++(queued ? sent : dropped);
    return queued;
  }

  std::uint16_t connection{kNoConnection};
  std::uint16_t value_handle{0U};
  std::uint32_t sent{0U};
  std::uint32_t dropped{0U};
};

// NimBLE retains these callback pointers for the lifetime of the host.
std::optional<ExtensibleLiveSignalStream> stream{};
NotificationSink sink{};
std::atomic<bool> telemetry_started{false};
ble_npl_callout sample_timer{};
ble_gap_event_listener gap_listener{};
bool bound{false};

LiveStreamClock monotonic_now() noexcept { return LiveStreamClock{esp_timer_get_time() / 1000}; }

// Round the delay up so a non-millisecond NPL tick cannot violate the 100 ms
// sampling interval. sample_and_notify() checks the deadline against the
// monotonic millisecond clock as a second guard.
ble_npl_time_t ticks_at_least(const LiveStreamClock wait) noexcept {
  const auto milliseconds = static_cast<std::uint32_t>(wait.count());
  ble_npl_time_t ticks = ble_npl_time_ms_to_ticks32(milliseconds);
  if (ble_npl_time_ticks_to_ms32(ticks) < milliseconds)
    ++ticks;
  return ticks;
}

void schedule_next_sample() noexcept {
  if (!stream.has_value()) {
    ble_npl_callout_stop(&sample_timer);
    return;
  }
  const auto wait = stream->time_until_sample(monotonic_now());
  if (!wait.has_value()) {
    ble_npl_callout_stop(&sample_timer);
    return;
  }
  ble_npl_callout_reset(&sample_timer, ticks_at_least(*wait));
}

// Re-read the accepted stored-bond decision instead of trusting a cached GAP
// event: this listener runs before the pairing listener has judged a new
// encryption event, and Clear bonds revokes the bond without a GAP event.
// The sample callback checks again immediately before any provider reads.
void refresh_connection_gates() noexcept {
  if (!stream.has_value())
    return;
  if (sink.connection == kNoConnection) {
    stream->set_link_secured(false);
    stream->set_catalog_token(0U);
    return;
  }
  stream->set_link_secured(link_has_accepted_bond(sink.connection));
  stream->set_catalog_token(signal_catalog_token_for_connection(sink.connection));
}

void sample_and_notify(ble_npl_event * /*event*/) {
  if (!stream.has_value())
    return;
  refresh_connection_gates();
  const LiveStreamClock now = monotonic_now();
  const auto wait = stream->time_until_sample(now);
  if (wait.has_value() && *wait == LiveStreamClock{0})
    (void)stream->offer(now, telemetry_started.load(std::memory_order_relaxed), sink);
  schedule_next_sample();
}

void log_connection_summary() noexcept {
  ESP_LOGI(
      kTag,
      "extensible live signals: %u packets queued, %u dropped; host task stack headroom: %u bytes",
      static_cast<unsigned>(sink.sent), static_cast<unsigned>(sink.dropped),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void open_connection(const std::uint16_t connection) noexcept {
  // Firmware currently allows one central. Ignore an unexpected second link
  // rather than letting it take over the first connection's catalog token or
  // stream state.
  if (sink.connection != kNoConnection)
    return;
  stream->reset_connection();
  sink.connection = connection;
  sink.sent = 0U;
  sink.dropped = 0U;
  stream->set_att_mtu(ble_att_mtu(connection));
  refresh_connection_gates();
}

void close_connection() noexcept {
  log_connection_summary();
  stream->reset_connection();
  sink.connection = kNoConnection;
}

bool for_this_connection(const std::uint16_t connection) noexcept {
  return connection == sink.connection && connection != kNoConnection;
}

void apply_gap_event(const ble_gap_event &event) noexcept {
  switch (event.type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event.connect.status == 0)
      open_connection(event.connect.conn_handle);
    return;
  case BLE_GAP_EVENT_DISCONNECT:
    if (for_this_connection(event.disconnect.conn.conn_handle))
      close_connection();
    return;
  case BLE_GAP_EVENT_MTU:
    if (for_this_connection(event.mtu.conn_handle))
      stream->set_att_mtu(event.mtu.value);
    return;
  case BLE_GAP_EVENT_SUBSCRIBE:
    // NimBLE also reports a bonded peer's CCCD restored during re-encryption.
    if (for_this_connection(event.subscribe.conn_handle) &&
        event.subscribe.attr_handle == sink.value_handle)
      stream->set_notifications_enabled(event.subscribe.cur_notify != 0U);
    return;
  default:
    return;
  }
}

int handle_gap_event(ble_gap_event *const event, void * /*argument*/) {
  if (stream.has_value()) {
    apply_gap_event(*event);
    refresh_connection_gates();
    schedule_next_sample();
  }
  return 0;
}

int access_extensible_live_signals(std::uint16_t /*connection*/, std::uint16_t /*attribute*/,
                                   ble_gatt_access_ctxt * /*context*/, void * /*argument*/) {
  // Notify-only: ATT never routes reads or writes here.
  return BLE_ATT_ERR_UNLIKELY;
}

} // namespace

CharacteristicDefinition extensible_live_signals_characteristic() noexcept {
  CharacteristicDefinition definition{kExtensibleLiveSignalsAttribute};
  definition.flags = BLE_GATT_CHR_F_NOTIFY;
  definition.access = access_extensible_live_signals;
  definition.value_handle = &sink.value_handle;
  return definition;
}

int bind_extensible_live_signals(const SignalCatalogExport &catalog,
                                 const vehicle_signals::SignalProvider &provider) noexcept {
  if (bound)
    return BLE_HS_EINVAL;

  stream.emplace(catalog, provider);
  const int callout_rc = ble_npl_callout_init(&sample_timer, nimble_port_get_dflt_eventq(),
                                              sample_and_notify, nullptr);
  if (callout_rc != 0) {
    stream.reset();
    return BLE_HS_ENOMEM;
  }

  const int listener_rc = ble_gap_event_listener_register(&gap_listener, handle_gap_event, nullptr);
  if (listener_rc != 0) {
    ble_npl_callout_deinit(&sample_timer);
    stream.reset();
    return listener_rc;
  }
  bound = true;
  return 0;
}

void mark_extensible_live_signals_telemetry_started() noexcept {
  telemetry_started.store(true, std::memory_order_relaxed);
}

} // namespace companion_ble::internal
