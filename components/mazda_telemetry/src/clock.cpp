#include "mazda/publication_store.hpp"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace mazda::internal {

vehicle_core::MonotonicTimestamp SteadyClock::now() const noexcept {
#if defined(ESP_PLATFORM)
  // CAN acquisition and the application clock both use ESP Timer's
  // boot-scoped monotonic microseconds. Keeping this default clock on the
  // same source avoids an epoch correction between observations and reads.
  return static_cast<vehicle_core::MonotonicTimestamp>(esp_timer_get_time());
#else
  // The host implementation remains a monotonic microsecond clock so the
  // production default stays usable without ESP-IDF. Host composition tests
  // can still inject a deterministic vehicle_core::MonotonicClock.
  const auto duration = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<vehicle_core::MonotonicTimestamp>(
      std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
#endif
}

} // namespace mazda::internal
