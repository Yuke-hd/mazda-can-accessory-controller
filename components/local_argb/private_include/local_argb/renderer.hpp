#pragma once

#include "local_argb/lighting_sink.hpp"
#include "local_argb/local_argb.h"

namespace local_argb::internal {

inline constexpr Rgb kOnboardTurnStatus{128, 16, 0};
inline constexpr Rgb kOnboardBrakeStatus{kBrightnessCeiling, 0, 0};

constexpr bool frame_region_has_color(const PixelFrame &frame, const std::size_t start,
                                      const std::size_t count) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    if (frame[start + index] != kBlack) {
      return true;
    }
  }
  return false;
}

// The embedded board exposes one physical pixel while the renderer owns the
// full 100-pixel semantic frame. Collapse that frame to a visible diagnostic
// status without changing the renderer's animation or region ownership.
constexpr Rgb onboard_status_color(const PixelFrame &frame) noexcept {
  const bool brake_active = frame_region_has_color(frame, kBrakeLedStart, kBrakeLedCount);
  const bool turn_active = frame_region_has_color(frame, 0, kTurnLedCount) ||
                           frame_region_has_color(frame, kRightTurnLedStart, kTurnLedCount);
  if (brake_active) {
    // A single pixel cannot show both effects; keep the safety-critical brake
    // indication visible when a turn and brake frame overlap.
    return kOnboardBrakeStatus;
  }
  return turn_active ? kOnboardTurnStatus : kBlack;
}

struct AnimationContext {
  bool left_turn{false};
  bool right_turn{false};
  vehicle_core::Microseconds elapsed_us{0};
};

// Animation strategies are function pointers rather than owned objects so a
// controller can swap animations without a heap allocation or a new
// controller implementation. Strategies only draw into the supplied frame.
using AnimationStrategy = void (*)(PixelFrame &, const AnimationContext &) noexcept;

void render_running_flow(PixelFrame &frame, const AnimationContext &context) noexcept;
void render_center_out_fill(PixelFrame &frame, const AnimationContext &context) noexcept;

inline constexpr AnimationStrategy kRunningFlowAnimation = &render_running_flow;
inline constexpr AnimationStrategy kCenterOutFillAnimation = &render_center_out_fill;

// Generic strip renderer state machine. It is implementation-only so
// ordinary users cannot acquire renderer queue/task ownership.
class RendererController {
public:
  explicit RendererController(PixelFrameSink &sink,
                              AnimationStrategy animation = kRunningFlowAnimation) noexcept
      : sink_(&sink), animation_(animation != nullptr ? animation : kRunningFlowAnimation) {}

  void set_animation(AnimationStrategy animation) noexcept {
    animation_ = animation != nullptr ? animation : kRunningFlowAnimation;
  }

  bool start() noexcept;
  bool apply(LightingCommand command, vehicle_core::MonotonicTimestamp now_us) noexcept;
  bool tick(vehicle_core::MonotonicTimestamp now_us) noexcept;
  [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
  [[nodiscard]] PixelFrame frame_for(vehicle_core::MonotonicTimestamp now_us) const noexcept;
  bool write_desired(const PixelFrame &desired) noexcept;

  PixelFrameSink *sink_;
  LightingCommand command_{};
  bool has_command_{false};
  PixelFrame last_written_{};
  bool has_last_written_{false};
  bool faulted_{false};
  vehicle_core::MonotonicTimestamp animation_started_us_{0};
  AnimationStrategy animation_;
};

// Portable model of the embedded length-one overwrite queue.
class Mailbox {
public:
  void submit(LightingCommand command) noexcept {
    pending_ = command;
    has_pending_ = true;
  }
  [[nodiscard]] bool take(LightingCommand &command) noexcept {
    if (!has_pending_) {
      return false;
    }
    command = pending_;
    has_pending_ = false;
    return true;
  }

private:
  LightingCommand pending_{};
  bool has_pending_{false};
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
    if (armed_)
      last_progress_us_ = now_us;
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

} // namespace local_argb::internal
