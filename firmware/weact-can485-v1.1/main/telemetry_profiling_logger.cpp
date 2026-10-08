#include "telemetry_profiling_logger.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstddef>
#include <cstdint>

namespace weact_can485::telemetry_profiling {
namespace {

constexpr char kTag[] = "telemetry_profile";
constexpr std::uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
constexpr std::uint32_t kPollPeriodMs = 100;

const mazda::TelemetryProfileMetric &metric(const mazda::TelemetryProfileSnapshot &snapshot,
                                            const mazda::TelemetryProfileStage stage) noexcept {
  return snapshot.stages[static_cast<std::size_t>(stage)];
}

} // namespace

Logger::Logger(const mazda::VehicleTelemetry &telemetry) noexcept : telemetry_(&telemetry) {}

bool Logger::start() noexcept {
  return xTaskCreate(&Logger::task_entry, "tel_prof", kTaskStackBytes, this, kTaskPriority,
                     nullptr) == pdPASS;
}

void Logger::task_entry(void *const context) noexcept { static_cast<Logger *>(context)->run(); }

void Logger::run() noexcept {
  for (;;) {
    const auto sample_timestamp_us = static_cast<std::uint64_t>(esp_timer_get_time());
    poll(sample_timestamp_us);
    vTaskDelay(pdMS_TO_TICKS(kPollPeriodMs));
  }
}

void Logger::poll(const std::uint64_t sample_timestamp_us) noexcept {
  const auto snapshot = telemetry_->telemetry_profile_snapshot();
  if (!snapshot.enabled || snapshot.interval_end_us == 0 ||
      snapshot.interval_end_us == last_interval_end_us_)
    return;

  const auto &queue_wait = metric(snapshot, mazda::TelemetryProfileStage::QueueWait);
  const auto &pre_decode = metric(snapshot, mazda::TelemetryProfileStage::PreDecodeDue);
  const auto &decode = metric(snapshot, mazda::TelemetryProfileStage::Decode);
  const auto &diagnostics = metric(snapshot, mazda::TelemetryProfileStage::Diagnostics);
  const auto &publication = metric(snapshot, mazda::TelemetryProfileStage::Publication);
  const auto &notification = metric(snapshot, mazda::TelemetryProfileStage::NotificationEvaluation);
  const auto &lighting = metric(snapshot, mazda::TelemetryProfileStage::LightingEvaluation);
  const auto &total = metric(snapshot, mazda::TelemetryProfileStage::Total);

  // One bounded record per completed interval. Values are wall-clock elapsed
  // microseconds, not task CPU time; task preemption can inflate a sample.
  ESP_LOGI(kTag,
           "TELPROF v=1 wall_clock=elapsed_us interval_start_us=%llu interval_end_us=%llu "
           "interval_us=%llu sample_us=%llu "
           "metric_fields=calls,total_us,avg_us,p50_us,p95_us,p99_us,max_us "
           "queue_wait=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "pre_decode_due=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "decode=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "diagnostics=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "publication=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "notification=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "lighting=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
           "total=%llu/%llu/%llu/%llu/%llu/%llu/%llu",
           static_cast<unsigned long long>(snapshot.interval_start_us),
           static_cast<unsigned long long>(snapshot.interval_end_us),
           static_cast<unsigned long long>(snapshot.interval_duration_us),
           static_cast<unsigned long long>(sample_timestamp_us),
           static_cast<unsigned long long>(queue_wait.calls),
           static_cast<unsigned long long>(queue_wait.total_elapsed_us),
           static_cast<unsigned long long>(queue_wait.average_elapsed_us),
           static_cast<unsigned long long>(queue_wait.p50_elapsed_us),
           static_cast<unsigned long long>(queue_wait.p95_elapsed_us),
           static_cast<unsigned long long>(queue_wait.p99_elapsed_us),
           static_cast<unsigned long long>(queue_wait.max_elapsed_us),
           static_cast<unsigned long long>(pre_decode.calls),
           static_cast<unsigned long long>(pre_decode.total_elapsed_us),
           static_cast<unsigned long long>(pre_decode.average_elapsed_us),
           static_cast<unsigned long long>(pre_decode.p50_elapsed_us),
           static_cast<unsigned long long>(pre_decode.p95_elapsed_us),
           static_cast<unsigned long long>(pre_decode.p99_elapsed_us),
           static_cast<unsigned long long>(pre_decode.max_elapsed_us),
           static_cast<unsigned long long>(decode.calls),
           static_cast<unsigned long long>(decode.total_elapsed_us),
           static_cast<unsigned long long>(decode.average_elapsed_us),
           static_cast<unsigned long long>(decode.p50_elapsed_us),
           static_cast<unsigned long long>(decode.p95_elapsed_us),
           static_cast<unsigned long long>(decode.p99_elapsed_us),
           static_cast<unsigned long long>(decode.max_elapsed_us),
           static_cast<unsigned long long>(diagnostics.calls),
           static_cast<unsigned long long>(diagnostics.total_elapsed_us),
           static_cast<unsigned long long>(diagnostics.average_elapsed_us),
           static_cast<unsigned long long>(diagnostics.p50_elapsed_us),
           static_cast<unsigned long long>(diagnostics.p95_elapsed_us),
           static_cast<unsigned long long>(diagnostics.p99_elapsed_us),
           static_cast<unsigned long long>(diagnostics.max_elapsed_us),
           static_cast<unsigned long long>(publication.calls),
           static_cast<unsigned long long>(publication.total_elapsed_us),
           static_cast<unsigned long long>(publication.average_elapsed_us),
           static_cast<unsigned long long>(publication.p50_elapsed_us),
           static_cast<unsigned long long>(publication.p95_elapsed_us),
           static_cast<unsigned long long>(publication.p99_elapsed_us),
           static_cast<unsigned long long>(publication.max_elapsed_us),
           static_cast<unsigned long long>(notification.calls),
           static_cast<unsigned long long>(notification.total_elapsed_us),
           static_cast<unsigned long long>(notification.average_elapsed_us),
           static_cast<unsigned long long>(notification.p50_elapsed_us),
           static_cast<unsigned long long>(notification.p95_elapsed_us),
           static_cast<unsigned long long>(notification.p99_elapsed_us),
           static_cast<unsigned long long>(notification.max_elapsed_us),
           static_cast<unsigned long long>(lighting.calls),
           static_cast<unsigned long long>(lighting.total_elapsed_us),
           static_cast<unsigned long long>(lighting.average_elapsed_us),
           static_cast<unsigned long long>(lighting.p50_elapsed_us),
           static_cast<unsigned long long>(lighting.p95_elapsed_us),
           static_cast<unsigned long long>(lighting.p99_elapsed_us),
           static_cast<unsigned long long>(lighting.max_elapsed_us),
           static_cast<unsigned long long>(total.calls),
           static_cast<unsigned long long>(total.total_elapsed_us),
           static_cast<unsigned long long>(total.average_elapsed_us),
           static_cast<unsigned long long>(total.p50_elapsed_us),
           static_cast<unsigned long long>(total.p95_elapsed_us),
           static_cast<unsigned long long>(total.p99_elapsed_us),
           static_cast<unsigned long long>(total.max_elapsed_us));
  last_interval_end_us_ = snapshot.interval_end_us;
}

} // namespace weact_can485::telemetry_profiling
