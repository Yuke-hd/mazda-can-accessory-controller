#pragma once

#include <chrono>

namespace replay {

// The host player's only wall-time source. Replay time never reads it: the
// scheduler, controller, and renderer stay in replay-time units, and a pacer
// behind a ReplayGate is the one place that maps replay time to wall time.
class WallClock {
public:
  virtual ~WallClock() = default;
  // Monotonic wall time since an arbitrary epoch.
  [[nodiscard]] virtual std::chrono::microseconds now() const = 0;
  // Blocks until now() reaches deadline; returns immediately for past deadlines.
  virtual void sleep_until(std::chrono::microseconds deadline) = 0;
};

class SteadyWallClock final : public WallClock {
public:
  [[nodiscard]] std::chrono::microseconds now() const override;
  void sleep_until(std::chrono::microseconds deadline) override;
};

} // namespace replay
