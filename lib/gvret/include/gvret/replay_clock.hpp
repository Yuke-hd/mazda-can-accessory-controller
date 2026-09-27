#pragma once

#include <atomic>

#include "vehicle_core/time.hpp"

namespace gvret {

// A host-controlled monotonic clock for deterministic replay. The clock never
// reads wall time or advances itself; a replay scheduler decides when time may
// move so other scheduled work can run before the next frame timestamp.
class ReplayClock final : public vehicle_core::MonotonicClock {
public:
  explicit ReplayClock(vehicle_core::MonotonicTimestamp initial_time_us = 0) noexcept;

  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override;

  // Advance directly to target_time_us. Equal timestamps are accepted.
  // Regressions are rejected and leave the current time unchanged.
  [[nodiscard]] bool advance_to(vehicle_core::MonotonicTimestamp target_time_us) noexcept;

private:
  std::atomic<vehicle_core::MonotonicTimestamp> now_us_;
};

} // namespace gvret
