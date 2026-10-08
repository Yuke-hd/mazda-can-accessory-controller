#include "board/board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mazda/telemetry_synthetic_benchmark.hpp"
#include "sdkconfig.h"

#if CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS
#include "freertos_runtime_stats_logger.hpp"
#endif

#include <cstddef>
#include <cstdint>

namespace {
constexpr char kTag[] = "weact_telemetry_bench";
// The normal app_main task is 3,584 B and also carries the ESP-IDF startup
// call chain. Keep the benchmark's service-start and publication call path on
// a dedicated task, like the production vehicle-I/O startup sequence.
constexpr std::uint32_t kBenchmarkTaskStackBytes = 6144;
constexpr UBaseType_t kBenchmarkTaskPriority = tskIDLE_PRIORITY + 1;
constexpr std::uint32_t kMinimumStackHeadroomBytes = 1024;

#if CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS
weact_can485::freertos_runtime_stats::PauseSnapshot
read_benchmark_pause_snapshot(const void *) noexcept {
  const auto progress = mazda::benchmark::synthetic_benchmark_progress();
  return {progress.started, progress.work_budget_pauses, progress.requested_pause_ms, false, 0};
}
weact_can485::freertos_runtime_stats::Logger freertos_runtime_stats_logger{
    &read_benchmark_pause_snapshot, nullptr};
#endif

void benchmark_task(void *) noexcept {
  const auto result = mazda::benchmark::run_synthetic_benchmark();
  const auto stack_headroom_bytes = uxTaskGetStackHighWaterMark(nullptr);
  const auto &total =
      result.profile.stages[static_cast<std::size_t>(mazda::TelemetryProfileStage::Total)];
  if (stack_headroom_bytes < kMinimumStackHeadroomBytes) {
    ESP_LOGE(kTag, "benchmark task stack headroom is low: %u bytes",
             static_cast<unsigned>(stack_headroom_bytes));
  }
  ESP_LOGI(kTag,
           "synthetic telemetry benchmark: profiling=%s started=%d timed_out=%d elapsed_us=%llu "
           "frames_received=%llu frames_processed=%llu work_budget_pauses=%llu profile_ready=%d "
           "profile_interval_us=%llu total_calls=%llu total_p50_us=%llu total_p95_us=%llu "
           "total_p99_us=%llu stack_headroom_bytes=%u",
#if CONFIG_WEACT_CAN_TELEMETRY_PROFILING
           "on",
#else
           "off",
#endif
           result.started, result.timed_out, static_cast<unsigned long long>(result.elapsed_us),
           static_cast<unsigned long long>(result.frames_received),
           static_cast<unsigned long long>(result.frames_processed),
           static_cast<unsigned long long>(result.work_budget_pauses), result.profile_ready,
           static_cast<unsigned long long>(result.profile.interval_duration_us),
           static_cast<unsigned long long>(total.calls),
           static_cast<unsigned long long>(total.p50_elapsed_us),
           static_cast<unsigned long long>(total.p95_elapsed_us),
           static_cast<unsigned long long>(total.p99_elapsed_us),
           static_cast<unsigned>(stack_headroom_bytes));
  vTaskDelete(nullptr);
}
} // namespace

extern "C" void app_main() {
  if (!board::initialize_safe_defaults()) {
    ESP_LOGE(kTag, "safe GPIO initialization failed; benchmark not started");
    return;
  }

#if CONFIG_WEACT_CAN_FREERTOS_RUNTIME_STATS
  if (!freertos_runtime_stats_logger.start()) {
    ESP_LOGE(kTag, "FreeRTOS runtime diagnostic task could not be created; benchmark not started");
    return;
  }
#endif

  if (xTaskCreate(&benchmark_task, "telemetry_bench", kBenchmarkTaskStackBytes, nullptr,
                  kBenchmarkTaskPriority, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "benchmark task creation failed; benchmark not started");
  }
}
