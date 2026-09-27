#include "replay/wall_clock.hpp"

#include <thread>

namespace replay {

std::chrono::microseconds SteadyWallClock::now() const {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
}

void SteadyWallClock::sleep_until(const std::chrono::microseconds deadline) {
  std::this_thread::sleep_until(std::chrono::steady_clock::time_point{
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(deadline)});
}

} // namespace replay
