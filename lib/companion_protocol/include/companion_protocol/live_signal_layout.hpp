#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "vehicle_signals/signal_contracts.hpp"

// Live signals layout version 1: the normative signal table and choice-code
// tables. Slots are identified by catalog key; numeric SignalIds never reach
// the wire.

namespace companion_protocol {

inline constexpr std::size_t kLiveSignalCount = 19;
inline constexpr std::size_t kLiveFrameBytes = 23;

// One protocol choice code, mapped from a catalog choice key.
struct LiveChoiceCode {
  std::string_view key{};
  std::uint8_t code{0};
};

// One row of the signal table.
struct LiveSignalSlot {
  std::string_view key{};
  vehicle_signals::SignalType type{vehicle_signals::SignalType::Boolean};
  // Number and Enum: the field's byte offset. Boolean: the bit in `booleans`.
  std::uint8_t position{0};
  // Number only: wire units per engineering unit.
  float scale{0.0F};
  // Enum only: the choice-code table.
  const LiveChoiceCode *choices{nullptr};
  std::size_t choice_count{0};
  // The signal's freshness is intentionally unset by policy, so a Fresh
  // reading is a provider defect; it is reported as FreshnessUnverified
  // instead. Only vehicle.brake_pressed is flagged. A signal that merely has
  // no default timeout today, such as engine RPM, is not: a later reviewed
  // timeout may make its Fresh readings legitimate.
  bool freshness_unset{false};

  // The protocol code of an Enum choice key; std::nullopt when unmapped.
  [[nodiscard]] std::optional<std::uint8_t> choice_code(std::string_view choice_key) const noexcept;
};

// The signal table in normative index order.
[[nodiscard]] const std::array<LiveSignalSlot, kLiveSignalCount> &live_signal_slots() noexcept;

} // namespace companion_protocol
