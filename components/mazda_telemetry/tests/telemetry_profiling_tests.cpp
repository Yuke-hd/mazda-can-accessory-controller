#include "mazda/telemetry_profiling_internal.hpp"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void expect(const bool condition, const char *expression, const int line) {
  if (!condition) {
    std::cerr << __FILE__ << ':' << line << ": failed: " << expression << '\n';
    ++failures;
  }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

void test_histogram_and_percentiles() {
  mazda::internal::TelemetryStageProfiler profiler{};
  profiler.reset(100);
  profiler.record(mazda::TelemetryProfileStage::Decode, 100, 101);
  profiler.record(mazda::TelemetryProfileStage::Decode, 200, 202);
  profiler.record(mazda::TelemetryProfileStage::Decode, 300, 303);
  profiler.record(mazda::TelemetryProfileStage::Decode, 400, 404);

  EXPECT(!profiler.snapshot().interval_end_us);
  profiler.refresh(5'100'000);
  const auto snapshot = profiler.snapshot();
  const auto &decode =
      snapshot.stages[static_cast<std::size_t>(mazda::TelemetryProfileStage::Decode)];
  EXPECT(snapshot.enabled);
  EXPECT(snapshot.interval_start_us == 100);
  EXPECT(snapshot.interval_end_us == 5'100'000);
  EXPECT(snapshot.interval_duration_us == 5'099'900);
  EXPECT(decode.calls == 4);
  EXPECT(decode.total_elapsed_us == 10);
  EXPECT(decode.average_elapsed_us == 2);
  EXPECT(decode.p50_elapsed_us == 2);
  EXPECT(decode.p95_elapsed_us == 4);
  EXPECT(decode.p99_elapsed_us == 4);
  EXPECT(decode.max_elapsed_us == 4);
}

void test_interval_rollover_is_bounded_and_restarts() {
  mazda::internal::TelemetryStageProfiler profiler{};
  profiler.reset(1'000);
  profiler.record(mazda::TelemetryProfileStage::QueueWait, 1'000, 1'025);
  profiler.refresh(1'000 + mazda::internal::TelemetryStageProfiler::kIntervalDurationUs - 1);
  EXPECT(profiler.snapshot().interval_end_us == 0);

  profiler.refresh(1'000 + mazda::internal::TelemetryStageProfiler::kIntervalDurationUs);
  auto snapshot = profiler.snapshot();
  const auto &queue_wait =
      snapshot.stages[static_cast<std::size_t>(mazda::TelemetryProfileStage::QueueWait)];
  EXPECT(snapshot.interval_start_us == 1'000);
  EXPECT(snapshot.interval_end_us == 5'001'000);
  EXPECT(queue_wait.calls == 1);
  EXPECT(queue_wait.total_elapsed_us == 25);

  profiler.record(mazda::TelemetryProfileStage::QueueWait, 5'001'010, 5'001'012);
  profiler.refresh(5'001'001);
  snapshot = profiler.snapshot();
  EXPECT(snapshot.interval_end_us == 5'001'000);
  profiler.refresh(10'001'000);
  snapshot = profiler.snapshot();
  EXPECT(snapshot.interval_start_us == 5'001'000);
  EXPECT(snapshot.interval_end_us == 10'001'000);
  EXPECT(snapshot.stages[static_cast<std::size_t>(mazda::TelemetryProfileStage::QueueWait)].calls ==
         1);
}

void test_quantiles_scale_with_sample_count() {
  mazda::internal::TelemetryStageProfiler profiler{};
  profiler.reset(0);
  for (std::uint64_t elapsed = 1; elapsed <= 100; ++elapsed)
    profiler.record(mazda::TelemetryProfileStage::Decode, 0, elapsed);

  profiler.refresh(5'000'000);
  const auto snapshot = profiler.snapshot();
  const auto &decode =
      snapshot.stages[static_cast<std::size_t>(mazda::TelemetryProfileStage::Decode)];
  EXPECT(decode.calls == 100);
  EXPECT(decode.total_elapsed_us == 5'050);
  EXPECT(decode.average_elapsed_us == 50);
  EXPECT(decode.p50_elapsed_us == 64);
  EXPECT(decode.p95_elapsed_us == 128);
  EXPECT(decode.p99_elapsed_us == 128);
  EXPECT(decode.max_elapsed_us == 100);
}

} // namespace

int main() {
  test_histogram_and_percentiles();
  test_interval_rollover_is_bounded_and_restarts();
  test_quantiles_scale_with_sample_count();
  return failures == 0 ? 0 : 1;
}
