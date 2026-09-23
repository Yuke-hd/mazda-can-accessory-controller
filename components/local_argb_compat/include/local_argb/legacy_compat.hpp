#pragma once

// Temporary Stage 1 application compatibility. This header intentionally
// lives outside local_argb's ordinary public include root and is exposed only
// through the explicit firmware compatibility target until S3-A performs
// final application wiring. Ordinary renderer consumers must not include it.

#include <cstdint>

#include "local_argb/local_argb.h"
#include "mazda/state.hpp"
#include "vehicle_core/signal.hpp"

namespace local_argb {

inline constexpr vehicle_core::Microseconds kFailOffTimeoutUs = 250'000;
inline constexpr vehicle_core::Microseconds kPublishHeartbeatUs = 100'000;

inline constexpr Rgb kLeftGreen{0, kBrightnessCeiling, 0};
inline constexpr Rgb kRightBlue{0, 0, kBrightnessCeiling};
inline constexpr Rgb kHazardAmber{kBrightnessCeiling, kBrightnessCeiling / 2, 0};

enum class SemanticHealth : std::uint8_t { Online, CanOffline, DecoderError };

struct SemanticSnapshot {
  mazda::TurnState turn{mazda::TurnState::Unknown};
  vehicle_core::SignalStatus turn_status{vehicle_core::SignalStatus::Unknown};
  vehicle_core::MonotonicTimestamp turn_last_update_us{0};
  SemanticHealth health{SemanticHealth::CanOffline};
};

[[nodiscard]] SemanticSnapshot from_vehicle_state(const mazda::VehicleState &state,
                                                  SemanticHealth health) noexcept;
[[nodiscard]] Rgb color_for(const SemanticSnapshot &snapshot,
                            vehicle_core::MonotonicTimestamp now_us) noexcept;

// Legacy renderer model retained only for the pre-S3 application adapter.
class Controller {
public:
  explicit Controller(PixelSink &sink) noexcept : sink_(&sink) {}

  bool start() noexcept;
  bool apply(SemanticSnapshot snapshot, vehicle_core::MonotonicTimestamp now_us) noexcept;
  bool tick(vehicle_core::MonotonicTimestamp now_us) noexcept;
  [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
  bool write_desired(Rgb desired) noexcept;

  PixelSink *sink_;
  SemanticSnapshot snapshot_{};
  Rgb last_written_{};
  bool has_snapshot_{false};
  bool has_last_written_{false};
  bool faulted_{false};
};

class Mailbox {
public:
  void submit(SemanticSnapshot snapshot) noexcept {
    pending_ = snapshot;
    has_pending_ = true;
  }
  [[nodiscard]] bool take(SemanticSnapshot &snapshot) noexcept {
    if (!has_pending_) {
      return false;
    }
    snapshot = pending_;
    has_pending_ = false;
    return true;
  }

private:
  SemanticSnapshot pending_{};
  bool has_pending_{false};
};

class PublicationPolicy {
public:
  [[nodiscard]] bool should_publish(SemanticSnapshot snapshot,
                                    vehicle_core::MonotonicTimestamp now_us) noexcept;

private:
  SemanticSnapshot last_published_{};
  vehicle_core::MonotonicTimestamp last_publish_us_{0};
  bool has_published_{false};
};

class DriverWatchdog {
public:
  void begin(vehicle_core::MonotonicTimestamp now_us) noexcept {
    started_us_ = now_us;
    in_progress_ = true;
  }
  void end() noexcept { in_progress_ = false; }
  [[nodiscard]] bool restart_due(vehicle_core::MonotonicTimestamp now_us) const noexcept {
    return in_progress_ && (now_us < started_us_ || now_us - started_us_ > kDriverHangRestartUs);
  }

private:
  vehicle_core::MonotonicTimestamp started_us_{0};
  bool in_progress_{false};
};

class WorkerLease {
public:
  void arm(vehicle_core::MonotonicTimestamp now_us) noexcept {
    last_progress_us_ = now_us;
    armed_ = true;
  }
  void heartbeat(vehicle_core::MonotonicTimestamp now_us) noexcept {
    if (armed_) {
      last_progress_us_ = now_us;
    }
  }
  void disarm() noexcept { armed_ = false; }
  [[nodiscard]] bool restart_due(vehicle_core::MonotonicTimestamp now_us) const noexcept {
    return armed_ &&
           (now_us < last_progress_us_ || now_us - last_progress_us_ > kWorkerStallRestartUs);
  }

private:
  vehicle_core::MonotonicTimestamp last_progress_us_{0};
  bool armed_{false};
};

// Legacy application queue submission retained only for the current firmware
// adapter. The generic Stage 2 service uses the private sink contract.
bool submit(SemanticSnapshot snapshot) noexcept;

} // namespace local_argb
