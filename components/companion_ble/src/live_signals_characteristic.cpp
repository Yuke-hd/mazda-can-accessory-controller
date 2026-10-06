#include "companion_ble/live_signals_characteristic.hpp"

#include "companion_ble/security.hpp"
#include "companion_protocol/live_signal_stream.hpp"
#include "companion_protocol/live_signals.hpp"
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

// Every function here except bind_live_signals() and
// mark_live_signals_telemetry_started() runs on the NimBLE host
// task: GAP listener events and the sampling callout are both delivered there,
// so the stream state needs no lock. Nothing here waits: a frame the stack
// cannot queue is dropped and the latest state goes out at the next slot.

namespace companion_ble::internal {
namespace {

using companion_protocol::LiveFrame;
using companion_protocol::LiveSignalSampler;
using companion_protocol::LiveSignalStream;
using companion_protocol::LiveStreamClock;

constexpr const char *kTag = "companion_ble";

class NotificationSink final : public companion_protocol::LiveFrameSink {
public:
  bool try_queue(const LiveFrame &frame) noexcept override {
    os_mbuf *const payload =
        ble_hs_mbuf_from_flat(frame.data(), static_cast<std::uint16_t>(frame.size()));
    // The msys pool is exhausted: drop the frame instead of waiting.
    const bool queued =
        payload != nullptr && ble_gatts_notify_custom(connection, value_handle, payload) == 0;
    ++(queued ? sent : dropped);
    return queued;
  }

  std::uint16_t connection{BLE_HS_CONN_HANDLE_NONE};
  std::uint16_t value_handle{0U};
  std::uint32_t sent{0U};
  std::uint32_t dropped{0U};
};

// Static storage: NimBLE keeps pointers to the callout, the listener and the
// value handle for the lifetime of the host.
LiveSignalStream stream{};
NotificationSink sink{};
std::optional<LiveSignalSampler> sampler{};
// Written once by the vehicle I/O startup task on core 1 after telemetry
// starts, read on the host task.
std::atomic<bool> telemetry_started{false};
ble_npl_callout sample_timer{};
ble_gap_event_listener gap_listener{};

LiveStreamClock monotonic_now() noexcept { return LiveStreamClock{esp_timer_get_time() / 1000}; }

// The smallest tick count that is not shorter than `wait`. With this build's
// CONFIG_BT_NIMBLE_USE_ESP_TIMER=y the NPL tick is 1 ms and the callout runs
// on esp_timer, so the conversion is exact and the round-up never applies. It
// is kept as a defensive measure for the FreeRTOS-timer NPL backend, where a
// truncating conversion would fire early; sample_and_notify() re-checks the
// cap against esp_timer either way.
ble_npl_time_t ticks_at_least(const LiveStreamClock wait) noexcept {
  const auto milliseconds = static_cast<std::uint32_t>(wait.count());
  ble_npl_time_t ticks = ble_npl_time_ms_to_ticks32(milliseconds);
  if (ble_npl_time_ticks_to_ms32(ticks) < milliseconds)
    ++ticks;
  return ticks;
}

void schedule_next_sample() noexcept {
  const auto wait = stream.time_until_sample(monotonic_now());
  if (!wait.has_value()) {
    ble_npl_callout_stop(&sample_timer);
    return;
  }
  ble_npl_callout_reset(&sample_timer, ticks_at_least(*wait));
}

// The link gate is the pairing policy's accepted bond (ble-protocol.md,
// "Notification gate"), re-read before every decision instead of cached from
// one GAP event, for two reasons. NimBLE calls GAP listeners before the
// connection callback, so at ENC_CHANGE this listener runs before the pairing
// policy has judged the encryption. And Clear bonds revokes the accepted bond
// without any GAP event; re-reading it at the next sample stops the stream
// before the disconnect that follows.
void refresh_link_gate() noexcept {
  if (sink.connection != BLE_HS_CONN_HANDLE_NONE)
    stream.set_link_secured(link_has_accepted_bond(sink.connection));
}

void sample_and_notify(ble_npl_event * /*event*/) {
  refresh_link_gate();
  const LiveStreamClock now = monotonic_now();
  const auto wait = stream.time_until_sample(now);
  if (wait.has_value() && *wait == LiveStreamClock{0})
    (void)stream.offer(now, sampler->sample(telemetry_started.load(std::memory_order_relaxed)),
                       sink);
  schedule_next_sample();
}

void log_connection_summary() noexcept {
  ESP_LOGI(kTag, "live signals: %u frames queued, %u dropped; host task stack headroom: %u bytes",
           static_cast<unsigned>(sink.sent), static_cast<unsigned>(sink.dropped),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void open_connection(const std::uint16_t connection) noexcept {
  // The stream follows one central. The build allows one connection
  // (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1); should that grow, a second central
  // is ignored rather than taking over the first one's stream. It also never
  // starts streaming after the first central leaves, because its CONNECT was
  // already dropped: this fails closed, and supporting several centrals would
  // need per-connection stream state.
  if (sink.connection != BLE_HS_CONN_HANDLE_NONE)
    return;
  stream.reset_connection();
  sink.connection = connection;
  sink.sent = 0U;
  sink.dropped = 0U;
  stream.set_att_mtu(ble_att_mtu(connection));
}

void close_connection() noexcept {
  log_connection_summary();
  stream.reset_connection();
  sink.connection = BLE_HS_CONN_HANDLE_NONE;
}

// Every event but CONNECT applies only to the tracked connection.
bool for_this_connection(const std::uint16_t connection) noexcept {
  return connection == sink.connection && connection != BLE_HS_CONN_HANDLE_NONE;
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
      stream.set_att_mtu(event.mtu.value);
    return;
  case BLE_GAP_EVENT_SUBSCRIBE:
    // Includes a bonded peer's setting restored at re-encryption.
    if (for_this_connection(event.subscribe.conn_handle) &&
        event.subscribe.attr_handle == sink.value_handle)
      stream.set_notifications_enabled(event.subscribe.cur_notify != 0U);
    return;
  default:
    return;
  }
}

int handle_gap_event(ble_gap_event *const event, void * /*argument*/) {
  apply_gap_event(*event);
  refresh_link_gate();
  schedule_next_sample();
  return 0;
}

// Notify-only: the characteristic has no Read or Write property, so ATT never
// routes an access here.
int access_live_signals(std::uint16_t /*connection*/, std::uint16_t /*attribute*/,
                        ble_gatt_access_ctxt * /*context*/, void * /*argument*/) {
  return BLE_ATT_ERR_UNLIKELY;
}

} // namespace

CharacteristicDefinition live_signals_characteristic() noexcept {
  CharacteristicDefinition definition{CompanionAttribute::LiveSignals};
  definition.flags = BLE_GATT_CHR_F_NOTIFY;
  definition.access = access_live_signals;
  definition.value_handle = &sink.value_handle;
  return definition;
}

int bind_live_signals(const vehicle_signals::SignalProvider &provider) noexcept {
  sampler.emplace(provider);
  const int rc = ble_npl_callout_init(&sample_timer, nimble_port_get_dflt_eventq(),
                                      sample_and_notify, nullptr);
  if (rc != 0)
    return BLE_HS_ENOMEM;
  return ble_gap_event_listener_register(&gap_listener, handle_gap_event, nullptr);
}

void mark_live_signals_telemetry_started() noexcept {
  telemetry_started.store(true, std::memory_order_relaxed);
}

} // namespace companion_ble::internal
