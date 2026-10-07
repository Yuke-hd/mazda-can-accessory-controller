#pragma once

#include <cstdint>
#include <limits>

namespace weact_can485::freshness_debug {

// Keeps stale-transition serial output below one record per second while
// allowing the recorder's aggregate transition count to continue advancing.
class StaleLogRateLimiter final {
public:
  static constexpr std::uint64_t kMinimumPeriodUs = 1'000'000ULL;

  [[nodiscard]] bool permit(const std::uint64_t sample_timestamp_us) noexcept {
    const auto deadline =
        last_timestamp_us_ > std::numeric_limits<std::uint64_t>::max() - kMinimumPeriodUs
            ? std::numeric_limits<std::uint64_t>::max()
            : last_timestamp_us_ + kMinimumPeriodUs;
    if (has_last_timestamp_ && (last_timestamp_us_ == std::numeric_limits<std::uint64_t>::max() ||
                                sample_timestamp_us < deadline))
      return false;
    last_timestamp_us_ = sample_timestamp_us;
    has_last_timestamp_ = true;
    return true;
  }

private:
  bool has_last_timestamp_{false};
  std::uint64_t last_timestamp_us_{0};
};

} // namespace weact_can485::freshness_debug
