#include "gvret/replay_clock.hpp"

namespace gvret {

ReplayClock::ReplayClock(const vehicle_core::MonotonicTimestamp initial_time_us) noexcept
    : now_us_(initial_time_us) {}

vehicle_core::MonotonicTimestamp ReplayClock::now() const noexcept { return now_us_.load(); }

bool ReplayClock::advance_to(const vehicle_core::MonotonicTimestamp target_time_us) noexcept {
  auto observed_time_us = now_us_.load();
  while (target_time_us > observed_time_us) {
    if (now_us_.compare_exchange_weak(observed_time_us, target_time_us)) {
      return true;
    }
  }
  return target_time_us == observed_time_us;
}

} // namespace gvret
