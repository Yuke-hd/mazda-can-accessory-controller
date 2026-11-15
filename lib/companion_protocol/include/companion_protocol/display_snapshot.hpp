#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "companion_protocol/live_signal_layout.hpp"
#include "vehicle_signals/signal_catalog.hpp"
#include "vehicle_signals/signal_contracts.hpp"
#include "vehicle_signals/signal_provider.hpp"

// Round display ESP-NOW telemetry v1 ("ECAN"): one complete 200-byte snapshot
// of 23 signals. The wire contract is owned by the display project
// (esp32-1-75, docs/protocol/esp-now-v1.md). Slots are
// identified by catalog key; numeric SignalIds never reach the wire.

namespace companion_protocol {

inline constexpr std::size_t kDisplaySignalCount = 23;
inline constexpr std::size_t kDisplaySnapshotBytes = 200;

using DisplaySnapshot = std::array<std::uint8_t, kDisplaySnapshotBytes>;

// Availability codes of the display protocol; 7 extends SignalStatusCode.
enum class DisplayAvailability : std::uint8_t {
  NoData = 0,
  Fresh = 1,
  Stale = 2,
  FreshnessUnverified = 3,
  Unavailable = 4,
  ReadFailed = 5,
  NotSupported = 6,
  Unknown = 7,
};

// The values a Number slot may carry; anything else is sent as Unknown.
enum class DisplayRange : std::uint8_t { Signed, NonNegative, Fraction };

// One row of the signal table. Slot index i carries wire signal ID i + 1.
struct DisplaySignalSlot {
  std::string_view key{};
  vehicle_signals::SignalType type{vehicle_signals::SignalType::Boolean};
  DisplayRange range{DisplayRange::Signed};
  // Enum only: the choice-code table.
  const LiveChoiceCode *choices{nullptr};
  std::size_t choice_count{0};
  // The contract forbids Fresh here (brake pedal and pressure); a Fresh
  // reading is sent as FreshnessUnverified.
  bool freshness_unset{false};

  [[nodiscard]] std::optional<std::uint8_t> choice_code(std::string_view choice_key) const noexcept;
};

[[nodiscard]] const std::array<DisplaySignalSlot, kDisplaySignalCount> &
display_signal_slots() noexcept;

// Samples every slot through SignalProvider::read() only. Keys are resolved
// once at construction; a key missing from the catalog, with a different
// type, or without the Read capability is sent as NotSupported.
class DisplaySnapshotSampler final {
public:
  explicit DisplaySnapshotSampler(const vehicle_signals::SignalProvider &provider) noexcept;
  DisplaySnapshotSampler(const DisplaySnapshotSampler &) = delete;
  DisplaySnapshotSampler &operator=(const DisplaySnapshotSampler &) = delete;

  // One complete snapshot. With `telemetry_started` false every record is
  // Unknown with a zero value (the canonical stopped packet).
  [[nodiscard]] DisplaySnapshot sample(bool telemetry_started, std::uint32_t session,
                                       std::uint32_t sequence) const noexcept;

private:
  const vehicle_signals::SignalProvider &provider_;
  std::array<const vehicle_signals::SignalMetadata *, kDisplaySignalCount> resolved_{};
};

} // namespace companion_protocol
