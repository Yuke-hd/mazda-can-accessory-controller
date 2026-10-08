#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mazda {

enum class TelemetryProfileStage : std::uint8_t {
  QueueWait = 0,
  PreDecodeDue = 1,
  Decode = 2,
  Diagnostics = 3,
  Publication = 4,
  NotificationEvaluation = 5,
  LightingEvaluation = 6,
  Total = 7,
  Count = 8,
};

inline constexpr std::size_t kTelemetryProfileStageCount =
    static_cast<std::size_t>(TelemetryProfileStage::Count);

struct TelemetryProfileMetric final {
  std::uint64_t calls{0};
  std::uint64_t total_elapsed_us{0};
  std::uint64_t average_elapsed_us{0};
  std::uint64_t p50_elapsed_us{0};
  std::uint64_t p95_elapsed_us{0};
  std::uint64_t p99_elapsed_us{0};
  std::uint64_t max_elapsed_us{0};
};

// A value-only snapshot of bounded, approximate wall-clock measurements. The
// percentile values are upper bounds of fixed logarithmic histogram buckets.
// They are intentionally labelled elapsed time rather than task CPU time:
// preemption can inflate any individual sample.
struct TelemetryProfileSnapshot final {
  bool enabled{false};
  std::uint64_t interval_start_us{0};
  std::uint64_t interval_end_us{0};
  std::uint64_t interval_duration_us{0};
  std::array<TelemetryProfileMetric, kTelemetryProfileStageCount> stages{};
};

} // namespace mazda
