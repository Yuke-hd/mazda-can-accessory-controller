#include "freshness_debug_rate_limiter.hpp"

#include <cstdint>
#include <limits>

int main() {
  weact_can485::freshness_debug::StaleLogRateLimiter limiter{};
  if (!limiter.permit(100))
    return 1;
  if (limiter.permit(100 + 999'999))
    return 1;
  if (!limiter.permit(100 + 1'000'000))
    return 1;
  if (limiter.permit(100 + 1'000'001))
    return 1;
  // A backwards sample cannot bypass the one-second interval after a
  // permitted event.
  if (limiter.permit(100 + 500'000))
    return 1;
  weact_can485::freshness_debug::StaleLogRateLimiter wrapped{};
  if (!wrapped.permit(std::numeric_limits<std::uint64_t>::max()))
    return 1;
  if (wrapped.permit(std::numeric_limits<std::uint64_t>::max()))
    return 1;
  return 0;
}
