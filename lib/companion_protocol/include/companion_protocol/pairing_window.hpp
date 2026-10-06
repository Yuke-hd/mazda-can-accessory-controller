#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace companion_protocol {

// Monotonic milliseconds since boot, as the pairing policy measures time.
using PairingClock = std::chrono::milliseconds;

// A user key press opens the pairing window for this long (ble-protocol.md).
inline constexpr PairingClock kPairingWindowPeriod{120'000};

// Whether this boot can store bonds. NVS initialization failure makes it
// Unavailable: every pairing is rejected and the window never opens.
enum class BondStorage : std::uint8_t { Unavailable, Available };

// What the controller does with a pairing request from a central it already
// has a bond for.
enum class RepeatPairingAction : std::uint8_t {
  // Inside the window: delete the old bond before key exchange, then pair.
  DeleteOldBondAndRetry,
  // Outside the window: keep the bond, drop the request and the link.
  IgnoreAndDisconnect,
};

// The window in which the controller accepts a new pairing. It opens only on
// a debounced user key press, never at boot or on any reset, and closes when
// its period expires or after one new bond is stored.
class PairingWindow final {
public:
  explicit PairingWindow(BondStorage storage) noexcept : storage_{storage} {}

  // A debounced user key press. Opens the window, or restarts the period of
  // an open one. Returns false when the boot cannot store bonds.
  bool open(PairingClock now) noexcept;
  // One new bond was stored.
  void close() noexcept { closes_at_.reset(); }

  [[nodiscard]] bool is_open(PairingClock now) const noexcept;
  // When an open window expires; empty once it is closed.
  [[nodiscard]] std::optional<PairingClock> closes_at() const noexcept { return closes_at_; }
  [[nodiscard]] RepeatPairingAction repeat_pairing_action(PairingClock now) const noexcept;

private:
  BondStorage storage_;
  std::optional<PairingClock> closes_at_{};
};

} // namespace companion_protocol
