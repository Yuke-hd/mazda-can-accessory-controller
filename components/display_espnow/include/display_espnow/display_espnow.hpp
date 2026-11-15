#pragma once

// Round display ESP-NOW link (docs/specs/companion/display-espnow.md).
//
// Streams ECAN telemetry v1 snapshots to the display saved by pairing v1. The
// composition root hands it the read-only signal provider; it never touches
// CAN, the LED renderer or lighting. A failure only loses the display link.

#include "vehicle_signals/signal_provider.hpp"

namespace display_espnow {

struct Inputs {
  // False when NVS initialization failed this boot: no pairing can be read,
  // so Wi-Fi is never started.
  bool nvs_available{false};
};

// Starts the link on its own low-priority startup task and returns without
// waiting. `provider` must outlive the firmware. Call once, after NVS
// initialization. Returns false when start() already ran or the task could
// not be created; later failures are logged without key material.
[[nodiscard]] bool start(const vehicle_signals::SignalProvider &provider,
                         const Inputs &inputs) noexcept;

// Records that the telemetry facade started, so snapshots carry live
// readings instead of the canonical stopped packet. Safe from any task,
// before or after start().
void mark_telemetry_started() noexcept;

} // namespace display_espnow
