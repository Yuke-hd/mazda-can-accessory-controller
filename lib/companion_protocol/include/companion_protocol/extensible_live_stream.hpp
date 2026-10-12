#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "companion_protocol/extensible_live_signals.hpp"
#include "companion_protocol/live_signal_stream.hpp"

namespace companion_protocol {

// Port to the BLE stack for the catalog-keyed stream. Queueing is
// nonblocking; true means the stack retained this packet, not that the peer
// received it.
class ExtensibleLiveFrameSink {
public:
  virtual bool try_queue(const ExtensibleLiveSignalPacket &packet) noexcept = 0;

protected:
  ExtensibleLiveFrameSink() noexcept = default;
  ExtensibleLiveFrameSink(const ExtensibleLiveFrameSink &) = default;
  ExtensibleLiveFrameSink &operator=(const ExtensibleLiveFrameSink &) = default;
  ~ExtensibleLiveFrameSink() = default;
};

// Portable, allocation-free pacing for one extensible live-signal
// connection. The catalog and provider must outlive this object. Calls are
// serialized by the binding; this class is not thread-safe.
class ExtensibleLiveSignalStream final {
public:
  ExtensibleLiveSignalStream(const SignalCatalogExport &catalog,
                             const vehicle_signals::SignalProvider &provider) noexcept
      : catalog_(catalog), provider_(provider) {}

  void set_notifications_enabled(bool enabled) noexcept;
  // True only for encryption backed by an accepted stored bond.
  void set_link_secured(bool secured) noexcept;
  void set_att_mtu(std::uint16_t mtu) noexcept;
  // The per-connection catalog token. Zero makes the stream unavailable. A
  // changed token clears the accepted map and restarts the packet sequence.
  void set_catalog_token(std::uint32_t token) noexcept;
  // Suppresses sampling while a Config or Command write handler owns the BLE
  // host context. Resuming does not reset sequence or accepted baselines.
  void set_config_paused(bool paused) noexcept { config_paused_ = paused; }
  // A connection opened or closed: every gate, token, cursor and timing
  // baseline is cleared.
  void reset_connection() noexcept;

  [[nodiscard]] bool streaming() const noexcept;
  [[nodiscard]] std::optional<LiveStreamClock>
  time_until_sample(LiveStreamClock now) const noexcept;

  // Samples due catalog records and attempts at most one packet. Returns true
  // when a packet was offered to the sink, including when the sink refused it.
  bool offer(LiveStreamClock now, bool telemetry_started, ExtensibleLiveFrameSink &sink) noexcept;

private:
  static constexpr std::size_t kMaxRecordBytes = 9;

  struct Conditions {
    bool notifications_enabled{false};
    bool link_secured{false};
    std::uint16_t att_mtu{0};
    std::uint32_t catalog_token{0};
  };

  struct AcceptedRecord {
    std::array<std::uint8_t, kMaxRecordBytes> bytes{};
    std::uint8_t size{0};
    std::optional<LiveStreamClock> accepted_at{};
  };

  template <typename Update> void update_conditions(Update update) noexcept;
  void restart_stream() noexcept;
  [[nodiscard]] bool sample_due(LiveStreamClock now) const noexcept;
  [[nodiscard]] bool record_due(std::uint16_t handle,
                                const ExtensibleLiveSignalPacket &single_record,
                                LiveStreamClock now) const noexcept;
  [[nodiscard]] bool empty_packet_due(LiveStreamClock now) const noexcept;
  void remember_accepted_records(const ExtensibleLiveSignalPacket &packet,
                                 LiveStreamClock now) noexcept;

  const SignalCatalogExport &catalog_;
  const vehicle_signals::SignalProvider &provider_;
  Conditions conditions_{};
  std::array<AcceptedRecord, kMaxSignalCatalogSignals> accepted_{};
  std::optional<LiveStreamClock> last_sample_{};
  std::optional<LiveStreamClock> empty_packet_accepted_at_{};
  std::uint16_t next_sequence_{0};
  std::uint16_t next_handle_{1};
  bool config_paused_{false};
};

} // namespace companion_protocol
