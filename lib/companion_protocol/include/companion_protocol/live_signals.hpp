#pragma once

#include <array>
#include <cstdint>

#include "companion_protocol/live_signal_layout.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

namespace companion_protocol {

// Status nibble availability code (bits 0-2).
enum class SignalStatusCode : std::uint8_t {
  NoData = 0,
  Fresh = 1,
  Stale = 2,
  FreshnessUnverified = 3,
  Unavailable = 4,
  ReadFailed = 5,
  NotSupported = 6,
};

// Copies a provider Availability through the explicit table. Only Fresh maps
// to SignalStatusCode::Fresh.
[[nodiscard]] SignalStatusCode status_code(vehicle_signals::Availability availability) noexcept;

// The status of one read() failure: NotSupported for InvalidSignal and
// UnsupportedCapability, ReadFailed for any other status.
[[nodiscard]] SignalStatusCode status_code(vehicle_signals::SignalStatus failure) noexcept;

using LiveFrame = std::array<std::uint8_t, kLiveFrameBytes>;

// One sampling pass: every frame field except `sequence`. Equal contents mean
// no field other than the sequence changed.
class LiveSignalContent final {
public:
  LiveSignalContent() noexcept;

  // The frame bytes with `sequence` set.
  [[nodiscard]] LiveFrame frame(std::uint8_t sequence) const noexcept;

  friend bool operator==(const LiveSignalContent &left, const LiveSignalContent &right) noexcept {
    return left.bytes_ == right.bytes_;
  }
  friend bool operator!=(const LiveSignalContent &left, const LiveSignalContent &right) noexcept {
    return !(left == right);
  }

private:
  friend class LiveSignalSampler;

  LiveFrame bytes_{};
};

// Samples the signal table through SignalProvider::read() only: it never
// subscribes, starts or stops the provider. Keys are resolved to catalog
// entries once, at construction; a key missing from the catalog, with a
// different type, or without the Read capability is reported NotSupported.
class LiveSignalSampler final {
public:
  explicit LiveSignalSampler(const vehicle_signals::SignalProvider &provider) noexcept;
  LiveSignalSampler(const LiveSignalSampler &) = delete;
  LiveSignalSampler &operator=(const LiveSignalSampler &) = delete;

  // Reads every slot once. `telemetry_started` sets flags bit 0.
  [[nodiscard]] LiveSignalContent sample(bool telemetry_started) const noexcept;

private:
  const vehicle_signals::SignalProvider &provider_;
  std::array<const vehicle_signals::SignalMetadata *, kLiveSignalCount> resolved_{};
};

} // namespace companion_protocol
