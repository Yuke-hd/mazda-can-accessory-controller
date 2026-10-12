#pragma once

#include <cstddef>
#include <cstdint>

#include "companion_protocol/bytes.hpp"
#include "companion_protocol/live_signals.hpp"
#include "companion_protocol/signal_catalog_export.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace companion_protocol {

inline constexpr std::uint8_t kExtensibleLiveSignalFormat = 1;
inline constexpr std::size_t kExtensibleLiveSignalHeaderBytes = 8;
inline constexpr std::size_t kExtensibleLiveSignalRecordHeaderBytes = 5;
inline constexpr std::size_t kExtensibleLiveSignalMtuOverheadBytes = 3;
inline constexpr std::size_t kExtensibleLiveSignalMaxPacketBytes = 244;
inline constexpr std::uint8_t kExtensibleLiveSignalPacketFormat = kExtensibleLiveSignalFormat;
inline constexpr std::size_t kExtensibleLiveSignalPacketHeaderBytes =
    kExtensibleLiveSignalHeaderBytes;
inline constexpr std::size_t kMaxExtensibleLiveSignalPacketBytes =
    kExtensibleLiveSignalMaxPacketBytes;

using ExtensibleLiveSignalPacket = BoundedBytes<kExtensibleLiveSignalMaxPacketBytes>;
using EncodedExtensibleLiveSignalPacket = ExtensibleLiveSignalPacket;

// Builds one independently decodable packet for the extensible live-signal
// characteristic. The builder owns no catalog or reading storage; the
// provider and export must outlive it and remain immutable in the usual
// provider-catalog manner. A packet is limited to min(MTU - 3, 244) bytes.
// Records are appended atomically, so a record that does not fit is not
// partially written.
class ExtensibleLiveSignalPacketBuilder final {
public:
  ExtensibleLiveSignalPacketBuilder(const SignalCatalogExport &catalog,
                                    const vehicle_signals::SignalProvider &provider,
                                    std::uint32_t catalog_token, std::uint16_t sequence,
                                    bool telemetry_started, std::uint16_t att_mtu) noexcept;

  ExtensibleLiveSignalPacketBuilder(const vehicle_signals::SignalProvider &provider,
                                    const SignalCatalogExport &catalog, std::uint32_t catalog_token,
                                    std::uint16_t sequence, bool telemetry_started,
                                    std::uint16_t att_mtu) noexcept;

  ExtensibleLiveSignalPacketBuilder(const ExtensibleLiveSignalPacketBuilder &) = delete;
  ExtensibleLiveSignalPacketBuilder &operator=(const ExtensibleLiveSignalPacketBuilder &) = delete;

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] bool full() const noexcept { return valid_ && packet_.size() == packet_limit_; }
  [[nodiscard]] std::size_t size() const noexcept { return packet_.size(); }

  // Reads and appends one catalog entry. A failed provider read still emits a
  // record carrying the declared type and the mapped read-failure status.
  [[nodiscard]] bool append(std::uint16_t wire_handle) noexcept;

  [[nodiscard]] bool append(std::uint16_t wire_handle,
                            const vehicle_signals::SignalReading &reading) noexcept {
    return append_reading(wire_handle, reading);
  }

  // Appends a supplied successful reading. This is useful to callers that
  // have already performed the read while selecting records for a packet.
  [[nodiscard]] bool append_reading(std::uint16_t wire_handle,
                                    const vehicle_signals::SignalReading &reading) noexcept;

  // Appends a failed read with no value bytes and the provider failure mapped
  // through the legacy status table.
  [[nodiscard]] bool append_failure(std::uint16_t wire_handle,
                                    vehicle_signals::SignalStatus failure) noexcept;

  [[nodiscard]] const ExtensibleLiveSignalPacket &packet() const noexcept { return packet_; }
  [[nodiscard]] const ExtensibleLiveSignalPacket &frame() const noexcept { return packet_; }

private:
  [[nodiscard]] const vehicle_signals::SignalMetadata *
  metadata_for_handle(std::uint16_t wire_handle) const noexcept;
  [[nodiscard]] bool append_status(std::uint16_t wire_handle,
                                   const vehicle_signals::SignalMetadata &metadata,
                                   SignalStatusCode status, const std::uint8_t *value_bytes,
                                   std::size_t value_length) noexcept;

  const SignalCatalogExport &catalog_;
  const vehicle_signals::SignalProvider &provider_;
  ExtensibleLiveSignalPacket packet_{};
  std::uint64_t appended_handles_{0};
  std::size_t packet_limit_{0};
  bool valid_{false};
};

// Samples catalog entries in wire-handle order. The optional range form is a
// convenience for a future fair scheduler; it does not retain readings or
// perform any pacing itself.
class ExtensibleLiveSignalSampler final {
public:
  ExtensibleLiveSignalSampler(const SignalCatalogExport &catalog,
                              const vehicle_signals::SignalProvider &provider) noexcept
      : catalog_(catalog), provider_(provider) {}

  ExtensibleLiveSignalSampler(const vehicle_signals::SignalProvider &provider,
                              const SignalCatalogExport &catalog) noexcept
      : catalog_(catalog), provider_(provider) {}

  ExtensibleLiveSignalSampler(const ExtensibleLiveSignalSampler &) = delete;
  ExtensibleLiveSignalSampler &operator=(const ExtensibleLiveSignalSampler &) = delete;

  [[nodiscard]] ExtensibleLiveSignalPacket sample(std::uint32_t catalog_token,
                                                  std::uint16_t sequence, bool telemetry_started,
                                                  std::uint16_t att_mtu) const noexcept;

  [[nodiscard]] ExtensibleLiveSignalPacket sample(bool telemetry_started, std::uint16_t sequence,
                                                  std::uint32_t catalog_token,
                                                  std::uint16_t att_mtu) const noexcept {
    return sample(catalog_token, sequence, telemetry_started, att_mtu);
  }

  [[nodiscard]] ExtensibleLiveSignalPacket
  sample_range(std::uint32_t catalog_token, std::uint16_t sequence, bool telemetry_started,
               std::uint16_t att_mtu, std::uint16_t first_handle,
               std::uint16_t handle_count) const noexcept;

private:
  const SignalCatalogExport &catalog_;
  const vehicle_signals::SignalProvider &provider_;
};

// Convenience form for callers that do not need to retain a sampler.
[[nodiscard]] ExtensibleLiveSignalPacket
encode_extensible_live_signals(const SignalCatalogExport &catalog,
                               const vehicle_signals::SignalProvider &provider,
                               std::uint32_t catalog_token, std::uint16_t sequence,
                               bool telemetry_started, std::uint16_t att_mtu) noexcept;

} // namespace companion_protocol
