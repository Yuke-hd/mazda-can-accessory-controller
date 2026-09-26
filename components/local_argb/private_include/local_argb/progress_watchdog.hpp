#pragma once

#include <cstdint>

#include "local_argb/local_argb.h"
#include "vehicle_core/time.hpp"

namespace local_argb::internal {

enum class ProgressTransition : std::uint8_t { None, Stalled, Resumed };

// Watches a wrapping progress count sampled from outside the monitored loop.
// Only a change of the count is progress, so an idle loop that keeps passing
// is healthy. A stall is non-latching: the next change reports Resumed.
class ProgressWatchdog {
public:
  void arm(const std::uint32_t progress, const vehicle_core::MonotonicTimestamp now_us) noexcept {
    last_progress_ = progress;
    last_change_us_ = now_us;
    armed_ = true;
    stalled_ = false;
  }

  [[nodiscard]] ProgressTransition sample(const std::uint32_t progress,
                                          const vehicle_core::MonotonicTimestamp now_us) noexcept {
    if (!armed_)
      return ProgressTransition::None;
    if (progress != last_progress_)
      return record_progress(progress, now_us);
    if (stalled_ || !bound_exceeded(now_us))
      return ProgressTransition::None;
    stalled_ = true;
    return ProgressTransition::Stalled;
  }

  [[nodiscard]] bool stalled() const noexcept { return stalled_; }

private:
  ProgressTransition record_progress(const std::uint32_t progress,
                                     const vehicle_core::MonotonicTimestamp now_us) noexcept {
    last_progress_ = progress;
    last_change_us_ = now_us;
    const bool was_stalled = stalled_;
    stalled_ = false;
    return was_stalled ? ProgressTransition::Resumed : ProgressTransition::None;
  }

  [[nodiscard]] bool bound_exceeded(const vehicle_core::MonotonicTimestamp now_us) const noexcept {
    return now_us < last_change_us_ || now_us - last_change_us_ > kProgressStallFailOffUs;
  }

  vehicle_core::MonotonicTimestamp last_change_us_{0};
  std::uint32_t last_progress_{0};
  bool armed_{false};
  bool stalled_{false};
};

} // namespace local_argb::internal
