#include "freshness_debug_logger.hpp"

#include "driver/twai.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <limits>

namespace weact_can485::freshness_debug {
namespace {

constexpr char kTag[] = "can_freshness_debug";
constexpr std::uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;
constexpr std::uint32_t kPollPeriodMs = 100;
constexpr std::uint64_t kSummaryPeriodUs = 5'000'000ULL;

struct TwaiStatusSnapshot final {
  bool valid{false};
  esp_err_t error{ESP_FAIL};
  std::uint64_t sample_timestamp_us{0};
  twai_status_info_t status{};
};

std::uint64_t non_negative_delta(const std::uint64_t newer, const std::uint64_t older) noexcept {
  return newer >= older ? newer - older : 0;
}

std::uint64_t saturating_add(const std::uint64_t value, const std::uint64_t increment) noexcept {
  return value > std::numeric_limits<std::uint64_t>::max() - increment
             ? std::numeric_limits<std::uint64_t>::max()
             : value + increment;
}

const char *availability_name(const mazda::Availability availability) noexcept {
  switch (availability) {
  case mazda::Availability::NoData:
    return "NoData";
  case mazda::Availability::Fresh:
    return "Fresh";
  case mazda::Availability::Stale:
    return "Stale";
  case mazda::Availability::FreshnessUnverified:
    return "FreshnessUnverified";
  case mazda::Availability::Unavailable:
    return "Unavailable";
  }
  return "Unknown";
}

const char *transport_name(const vehicle_core::TransportHealth transport) noexcept {
  switch (transport) {
  case vehicle_core::TransportHealth::AwaitingTraffic:
    return "awaiting_traffic";
  case vehicle_core::TransportHealth::Live:
    return "live";
  case vehicle_core::TransportHealth::TimedOut:
    return "timed_out";
  case vehicle_core::TransportHealth::Faulted:
    return "faulted";
  case vehicle_core::TransportHealth::Stopped:
    return "stopped";
  }
  return "unknown";
}

const char *lifecycle_name(const mazda::LifecycleState lifecycle) noexcept {
  switch (lifecycle) {
  case mazda::LifecycleState::Stopped:
    return "stopped";
  case mazda::LifecycleState::Running:
    return "running";
  case mazda::LifecycleState::Stopping:
    return "stopping";
  case mazda::LifecycleState::Faulted:
    return "faulted";
  }
  return "unknown";
}

const char *sample_source_name(const mazda::DebugSampleSource source) noexcept {
  switch (source) {
  case mazda::DebugSampleSource::None:
    return "none";
  case mazda::DebugSampleSource::TelemetryObserverFrame:
    return "telemetry_observer_frame";
  case mazda::DebugSampleSource::TelemetryObserverTimeout:
    return "telemetry_observer_timeout";
  case mazda::DebugSampleSource::TelemetryDueCheck:
    return "telemetry_due_check";
  case mazda::DebugSampleSource::LoggerPoll:
    return "logger_poll";
  case mazda::DebugSampleSource::TwaiStatusPoll:
    return "twai_status_poll";
  }
  return "unknown";
}

TwaiStatusSnapshot read_twai_status(const std::uint64_t sample_timestamp_us) noexcept {
  TwaiStatusSnapshot result{};
  result.sample_timestamp_us = sample_timestamp_us;
  result.error = twai_get_status_info(&result.status);
  result.valid = result.error == ESP_OK;
  if (!result.valid)
    result.status = {};
  return result;
}

void log_snapshot_fields(const char *const event, const mazda::DebugSnapshot &snapshot,
                         const TwaiStatusSnapshot &twai, const std::uint64_t twai_since_event_us,
                         const std::uint64_t logger_snapshot_read_drops,
                         const std::uint64_t stale_events_coalesced,
                         const std::uint64_t stale_events_suppressed) noexcept {
  // Keep this as one key=value record. The stale snapshot's timestamps are
  // recorded by the telemetry path; TWAI fields are a read-only logger-time
  // sample and are labelled with their own sample timestamp and age.
  ESP_LOGI(
      kTag,
      "CANDBG v=1 event=%s enabled=%d has_frame=%d id=0x%03lx frame_count=%llu "
      "ctr=%u prev_ctr=%u expected_ctr=%u ctr_valid=%d ctr_ok=%d counter_gap_count=%llu "
      "has_previous_frame_timestamp=%d prev_frame_ts_us=%llu frame_ts_us=%llu "
      "interarrival_us=%llu process_ts_us=%llu "
      "rx_process_latency_us=%llu publish_ts_us=%llu process_publish_latency_us=%llu "
      "signal_has_update=%d signal_last_update_us=%llu now_us=%llu signal_age_us=%llu "
      "freshness_timeout_us=%llu has_freshness_timeout=%d availability=%s process_status=%u "
      "update_not_advanced=%d update_not_advanced_count=%llu esp_timer_us=%llu "
      "steady_clock_us=%llu "
      "diagnostics_sample_ts_us=%llu diagnostics_sample_source=%s "
      "clock_delta_us=%lld lifecycle=%s transport_health=%s frames_received=%llu "
      "frames_processed=%llu frames_dropped=%llu ring_overflow=%llu driver_errors=%llu "
      "core_missed_frames=%llu controller_resets=%llu bus_off_events=%llu "
      "has_transport_last_frame=%d transport_last_frame_age_us=%llu "
      "stale_transition_count=%llu recovery_count=%llu snapshot_read_drops=%llu "
      "logger_snapshot_read_drops=%llu "
      "recorder_write_contention=%llu recorder_publications=%llu "
      "twai_status_valid=%d twai_status_error=%d twai_status_sample_ts_us=%llu "
      "twai_status_since_event_us=%llu twai_state=%u twai_msgs_to_rx=%lu twai_msgs_to_tx=%lu "
      "twai_rx_error_counter=%lu twai_tx_error_counter=%lu twai_tx_failed_count=%lu "
      "twai_rx_missed=%lu twai_rx_overrun=%lu twai_arb_lost_count=%lu twai_bus_error_count=%lu "
      "stale_events_coalesced=%llu stale_events_suppressed=%llu",
      event, snapshot.enabled, snapshot.has_frame, static_cast<unsigned long>(snapshot.identifier),
      static_cast<unsigned long long>(snapshot.frame_count), snapshot.counter,
      snapshot.previous_counter, snapshot.expected_counter, snapshot.counter_valid,
      snapshot.counter_sequential, static_cast<unsigned long long>(snapshot.counter_gap_count),
      snapshot.has_previous_frame_timestamp,
      static_cast<unsigned long long>(snapshot.previous_frame_timestamp_us),
      static_cast<unsigned long long>(snapshot.frame_timestamp_us),
      static_cast<unsigned long long>(snapshot.interarrival_us),
      static_cast<unsigned long long>(snapshot.processing_timestamp_us),
      static_cast<unsigned long long>(snapshot.rx_process_latency_us),
      static_cast<unsigned long long>(snapshot.publication_timestamp_us),
      static_cast<unsigned long long>(snapshot.process_publish_latency_us),
      snapshot.signal_has_update, static_cast<unsigned long long>(snapshot.signal_last_update_us),
      static_cast<unsigned long long>(snapshot.now_us),
      static_cast<unsigned long long>(snapshot.signal_age_us),
      static_cast<unsigned long long>(snapshot.freshness_timeout_us),
      snapshot.has_freshness_timeout, availability_name(snapshot.availability),
      snapshot.process_status, snapshot.update_not_advanced,
      static_cast<unsigned long long>(snapshot.update_not_advanced_count),
      static_cast<unsigned long long>(snapshot.esp_timer_us),
      static_cast<unsigned long long>(snapshot.steady_clock_us),
      static_cast<unsigned long long>(snapshot.diagnostics_sample_timestamp_us),
      sample_source_name(snapshot.diagnostics_sample_source),
      static_cast<long long>(snapshot.clock_delta_us),
      lifecycle_name(snapshot.diagnostics.lifecycle),
      transport_name(snapshot.diagnostics.transport),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.frames_received),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.frames_processed),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.frames_dropped),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.queue_overflows),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.driver_errors),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.missed_frames),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.controller_resets),
      static_cast<unsigned long long>(snapshot.diagnostics.acquisition.bus_off_events),
      snapshot.has_transport_last_frame,
      static_cast<unsigned long long>(snapshot.transport_last_frame_age_us),
      static_cast<unsigned long long>(snapshot.stale_transition_count),
      static_cast<unsigned long long>(snapshot.recovery_count),
      static_cast<unsigned long long>(snapshot.snapshot_read_drops),
      static_cast<unsigned long long>(logger_snapshot_read_drops),
      static_cast<unsigned long long>(snapshot.recorder_write_contention),
      static_cast<unsigned long long>(snapshot.recorder_publications), twai.valid,
      static_cast<int>(twai.error), static_cast<unsigned long long>(twai.sample_timestamp_us),
      static_cast<unsigned long long>(twai_since_event_us),
      static_cast<unsigned>(twai.status.state), static_cast<unsigned long>(twai.status.msgs_to_rx),
      static_cast<unsigned long>(twai.status.msgs_to_tx),
      static_cast<unsigned long>(twai.status.rx_error_counter),
      static_cast<unsigned long>(twai.status.tx_error_counter),
      static_cast<unsigned long>(twai.status.tx_failed_count),
      static_cast<unsigned long>(twai.status.rx_missed_count),
      static_cast<unsigned long>(twai.status.rx_overrun_count),
      static_cast<unsigned long>(twai.status.arb_lost_count),
      static_cast<unsigned long>(twai.status.bus_error_count),
      static_cast<unsigned long long>(stale_events_coalesced),
      static_cast<unsigned long long>(stale_events_suppressed));
}

} // namespace

Logger::Logger(const mazda::VehicleTelemetry &telemetry) noexcept : telemetry_(&telemetry) {}

bool Logger::start() noexcept {
  return xTaskCreate(&Logger::task_entry, "can_dbg_log", kTaskStackBytes, this, kTaskPriority,
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
  const auto snapshot = telemetry_->debug_snapshot();
  // A false enabled flag means the value-copy recorder could not acquire its
  // short read lock. Retry on the next bounded tick and do not advance any
  // event or summary accounting from a partial/default record.
  if (!snapshot.enabled) {
    ++snapshot_read_drops_;
    return;
  }

  const auto stale_snapshot = telemetry_->debug_stale_snapshot();
  const auto twai = read_twai_status(sample_timestamp_us);

  if (!stale_snapshot.enabled) {
    // The stale cache uses the same non-blocking read handoff as the current
    // cache. A failed read is observable locally and the recorder's aggregate
    // count is emitted on the next successful current snapshot.
    ++snapshot_read_drops_;
  } else {
    const auto stale_transition_count = stale_snapshot.stale_transition_count;
    if (stale_transition_count < observed_stale_transition_count_) {
      // A recorder reset or a new telemetry lifecycle starts a new sequence;
      // do not reinterpret it as a burst of old stale events.
      observed_stale_transition_count_ = stale_transition_count;
    }

    if (stale_transition_count > observed_stale_transition_count_) {
      const auto events_since_poll = stale_transition_count - observed_stale_transition_count_;
      if (events_since_poll > 1)
        stale_events_coalesced_ += events_since_poll - 1;

      if (stale_log_rate_limiter_.permit(sample_timestamp_us)) {
        log_snapshot_fields(
            "stale", stale_snapshot, twai,
            non_negative_delta(twai.sample_timestamp_us, stale_snapshot.esp_timer_us),
            snapshot_read_drops_, stale_events_coalesced_, stale_events_suppressed_);
      } else {
        stale_events_suppressed_ += events_since_poll;
      }
      observed_stale_transition_count_ = stale_transition_count;
    }
  }

  if (next_summary_timestamp_us_ == 0 || sample_timestamp_us >= next_summary_timestamp_us_) {
    log_snapshot_fields("summary", snapshot, twai, 0, snapshot_read_drops_,
                        stale_events_coalesced_, stale_events_suppressed_);
    next_summary_timestamp_us_ = saturating_add(sample_timestamp_us, kSummaryPeriodUs);
  }
}

} // namespace weact_can485::freshness_debug
