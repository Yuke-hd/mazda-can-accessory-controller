#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "companion_protocol/display_snapshot.hpp"
#include "companion_protocol/user_key_debouncer.hpp"
#include "vehicle_signals/signal_provider.hpp"

// Portable pieces of the round display ESP-NOW link (pairing v1 responder and
// telemetry v1 sender). The ESP-IDF binding lives in components/display_espnow.

namespace companion_protocol {

// ---- Saved pairing ---------------------------------------------------------

inline constexpr std::uint8_t kPairingRecordVersion = 1;
inline constexpr std::size_t kPairingRecordBytes = 26;

// The one authenticated display peer. A stored record is the active pairing;
// the controller has no pending stage (pairing-v1: it persists after the
// authenticated key response and restarts).
struct PairingRecord {
  std::array<std::uint8_t, 6> peer{}; // display station MAC
  std::uint8_t channel{0};            // Wi-Fi channel 1-13
  std::array<std::uint8_t, 16> lmk{}; // first 16 bytes of the exchanged key
};

using PairingRecordBytes = std::array<std::uint8_t, kPairingRecordBytes>;

// Layout: version, MAC[6], channel, pairing profile (1), ECAN version (1), LMK[16].
[[nodiscard]] PairingRecordBytes encode_pairing_record(const PairingRecord &record) noexcept;

// std::nullopt for a wrong length or version, a zero or multicast MAC, a
// channel outside 1-13, an unknown profile/ECAN version, or an all-zero LMK.
[[nodiscard]] std::optional<PairingRecord> decode_pairing_record(const std::uint8_t *bytes,
                                                                 std::size_t size) noexcept;

// ---- Setup code ------------------------------------------------------------

inline constexpr std::size_t kSetupCodeChars = 16;
using SetupCode = std::array<char, kSetupCodeChars>;

// Removes spaces and hyphens and uppercases; requires exactly 16 hex digits.
// The result is the Security1 proof-of-possession string.
[[nodiscard]] std::optional<SetupCode> normalize_setup_code(std::string_view text) noexcept;

// ---- Long press ------------------------------------------------------------

// 5 s at the 20 ms user key sample period, counted from the first raw press
// sample of a press that the debouncer accepts.
inline constexpr std::size_t kLongPressSamples = 250;

// Fires once per press held for kLongPressSamples. A key held since boot
// never fires (UserKeyDebouncer semantics); any released sample cancels.
class LongPressDetector final {
public:
  [[nodiscard]] bool sample(UserKeyLevel level) noexcept;

private:
  UserKeyDebouncer debouncer_{};
  std::size_t held_{0};
  bool armed_{false};
};

// ---- Snapshot publication ----------------------------------------------------

// Owns the session and sequence of one publisher run.
class SnapshotPublisher final {
public:
  using RandomSource = std::uint32_t (*)() noexcept;

  // Draws the session from `random` until it is nonzero.
  SnapshotPublisher(const vehicle_signals::SignalProvider &provider, RandomSource random,
                    std::uint32_t first_sequence = 0) noexcept;
  SnapshotPublisher(const SnapshotPublisher &) = delete;
  SnapshotPublisher &operator=(const SnapshotPublisher &) = delete;

  // The next snapshot; the sequence advances on every call (every attempt).
  [[nodiscard]] DisplaySnapshot next() noexcept;
  // Safe from any task.
  void mark_telemetry_started() noexcept { started_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t session() const noexcept { return session_; }

private:
  DisplaySnapshotSampler sampler_;
  std::uint32_t session_{0};
  std::uint32_t sequence_{0};
  std::atomic<bool> started_{false};
};

} // namespace companion_protocol
