#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#include "mazda/telemetry_profiling.hpp"

namespace mazda::internal {

// Fixed-storage profiler for the synchronous telemetry worker. The worker
// updates the active histogram without taking a lock. A low-rate reader uses
// the published copy; refresh() swaps the bounded interval under a short lock.
// No frame data, allocation, or serial output belongs here.
class TelemetryStageProfiler final {
public:
  static constexpr std::uint64_t kIntervalDurationUs = 5'000'000;
  static constexpr std::size_t kHistogramBucketCount = 32;

  void reset(std::uint64_t now_us) noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    active_ = {};
    published_ = {};
    interval_start_us_ = now_us;
    next_refresh_us_ = saturating_add(now_us, kIntervalDurationUs);
    published_.enabled = true;
    published_.interval_start_us = now_us;
  }

  void record(TelemetryProfileStage stage, std::uint64_t started_us,
              std::uint64_t ended_us) noexcept {
    if (ended_us < started_us)
      return;
    const auto index = static_cast<std::size_t>(stage);
    if (index >= kTelemetryProfileStageCount)
      return;
    auto &metric = active_[index];
    metric.calls = saturating_add(metric.calls, 1);
    const auto elapsed = ended_us - started_us;
    metric.total_elapsed_us = saturating_add(metric.total_elapsed_us, elapsed);
    if (elapsed > metric.max_elapsed_us)
      metric.max_elapsed_us = elapsed;
    auto &bucket = metric.histogram[bucket_for(elapsed)];
    if (bucket != std::numeric_limits<std::uint32_t>::max())
      ++bucket;
  }

  void refresh(std::uint64_t now_us) noexcept {
    if (now_us < next_refresh_us_)
      return;
    std::lock_guard<std::mutex> lock{mutex_};
    if (now_us < next_refresh_us_)
      return;
    published_ = make_snapshot(active_, interval_start_us_, now_us);
    active_ = {};
    interval_start_us_ = now_us;
    next_refresh_us_ = saturating_add(now_us, kIntervalDurationUs);
  }

  [[nodiscard]] TelemetryProfileSnapshot snapshot() const noexcept {
    std::unique_lock<std::mutex> lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock())
      return {};
    return published_;
  }

private:
  struct Accumulator final {
    std::uint64_t calls{0};
    std::uint64_t total_elapsed_us{0};
    std::uint64_t max_elapsed_us{0};
    std::array<std::uint32_t, kHistogramBucketCount> histogram{};
  };

  static std::uint64_t saturating_add(std::uint64_t value, std::uint64_t increment) noexcept {
    return value > std::numeric_limits<std::uint64_t>::max() - increment
               ? std::numeric_limits<std::uint64_t>::max()
               : value + increment;
  }

  static std::uint64_t saturating_multiply(std::uint64_t value, std::uint64_t multiplier) noexcept {
    return multiplier != 0 && value > std::numeric_limits<std::uint64_t>::max() / multiplier
               ? std::numeric_limits<std::uint64_t>::max()
               : value * multiplier;
  }

  static std::size_t bucket_for(std::uint64_t elapsed_us) noexcept {
    std::size_t bucket = 0;
    std::uint64_t upper_bound = 1;
    while (elapsed_us > upper_bound && bucket + 1 < kHistogramBucketCount) {
      upper_bound <<= 1;
      ++bucket;
    }
    return bucket;
  }

  static std::uint64_t bucket_upper_bound(std::size_t bucket) noexcept {
    if (bucket == 0)
      return 1;
    if (bucket >= kHistogramBucketCount - 1)
      return std::numeric_limits<std::uint64_t>::max();
    return std::uint64_t{1} << bucket;
  }

  static std::uint64_t percentile(const Accumulator &metric, std::uint64_t rank_numerator,
                                  std::uint64_t rank_denominator) noexcept {
    if (metric.calls == 0)
      return 0;
    const auto quotient = metric.calls / rank_denominator;
    const auto remainder = metric.calls % rank_denominator;
    // ceil(calls * numerator / denominator), evaluated in parts so a
    // saturated call count cannot overflow the multiplication.
    const auto remainder_rank =
        (remainder * rank_numerator + rank_denominator - 1) / rank_denominator;
    const auto rank = std::max<std::uint64_t>(
        1, saturating_add(saturating_multiply(quotient, rank_numerator), remainder_rank));
    std::uint64_t cumulative = 0;
    for (std::size_t bucket = 0; bucket < kHistogramBucketCount; ++bucket) {
      cumulative = saturating_add(cumulative, metric.histogram[bucket]);
      if (cumulative >= rank)
        return bucket_upper_bound(bucket);
    }
    return bucket_upper_bound(kHistogramBucketCount - 1);
  }

  static TelemetryProfileSnapshot
  make_snapshot(const std::array<Accumulator, kTelemetryProfileStageCount> &accumulators,
                std::uint64_t interval_start_us, std::uint64_t interval_end_us) noexcept {
    TelemetryProfileSnapshot result{};
    result.enabled = true;
    result.interval_start_us = interval_start_us;
    result.interval_end_us = interval_end_us;
    result.interval_duration_us =
        interval_end_us >= interval_start_us ? interval_end_us - interval_start_us : 0;
    for (std::size_t index = 0; index < kTelemetryProfileStageCount; ++index) {
      const auto &source = accumulators[index];
      auto &destination = result.stages[index];
      destination.calls = source.calls;
      destination.total_elapsed_us = source.total_elapsed_us;
      destination.average_elapsed_us =
          source.calls == 0 ? 0 : source.total_elapsed_us / source.calls;
      destination.p50_elapsed_us = percentile(source, 50, 100);
      destination.p95_elapsed_us = percentile(source, 95, 100);
      destination.p99_elapsed_us = percentile(source, 99, 100);
      destination.max_elapsed_us = source.max_elapsed_us;
    }
    return result;
  }

  mutable std::mutex mutex_{};
  std::array<Accumulator, kTelemetryProfileStageCount> active_{};
  TelemetryProfileSnapshot published_{};
  std::uint64_t interval_start_us_{0};
  std::uint64_t next_refresh_us_{0};
};

} // namespace mazda::internal
