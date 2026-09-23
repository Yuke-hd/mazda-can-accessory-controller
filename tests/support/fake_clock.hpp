#pragma once

#include "vehicle_core/time.hpp"

namespace test_support {

class FakeClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override { return now_us_; }
  void set(vehicle_core::MonotonicTimestamp timestamp_us) noexcept { now_us_ = timestamp_us; }
  void advance(vehicle_core::Microseconds delta_us) noexcept { now_us_ += delta_us; }

private:
  vehicle_core::MonotonicTimestamp now_us_{0};
};

} // namespace test_support
