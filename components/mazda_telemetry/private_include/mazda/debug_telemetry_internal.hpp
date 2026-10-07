#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include <optional>

#include "mazda/debug_telemetry.hpp"
#include "mazda/state.hpp"
#include "vehicle_telemetry/observer.hpp"

namespace mazda::internal {

// The recorder is deliberately separate from decoder and publication state.
// It owns no task, queue or allocation. The CAN worker updates its own
// evidence state without a mutex; a non-blocking cache handoff gives the
// low-rate logger a coherent snapshot when the reader is available.
class DebugRecorder final {
public:
  void reset() noexcept;

  [[nodiscard]] bool record_processed(const vehicle_core::RawCanFrame &frame,
                                      vehicle_telemetry::ProcessStatus status,
                                      vehicle_core::MonotonicTimestamp processing_timestamp_us,
                                      bool update_not_advanced) noexcept;

  [[nodiscard]] bool record_published(
      const VehicleState &state, const Diagnostics &diagnostics,
      vehicle_core::MonotonicTimestamp now_us,
      vehicle_core::MonotonicTimestamp publication_timestamp_us,
      vehicle_core::MonotonicTimestamp esp_timer_us,
      vehicle_core::MonotonicTimestamp steady_clock_us,
      std::optional<vehicle_core::MonotonicTimestamp> transport_last_frame_us,
      vehicle_core::MonotonicTimestamp diagnostic_sample_timestamp_us = 0,
      DebugSampleSource diagnostic_sample_source = DebugSampleSource::None) noexcept;

  [[nodiscard]] DebugSnapshot snapshot() const noexcept;
  [[nodiscard]] DebugSnapshot stale_snapshot() const noexcept;
  [[nodiscard]] std::uint64_t snapshot_read_drops() const noexcept {
    return snapshot_read_drops_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t recorder_write_contention() const noexcept {
    return recorder_write_contention_.load(std::memory_order_relaxed);
  }
#if !defined(ESP_PLATFORM)
  // Test-only contention seam. It models a logger task being descheduled
  // while holding the read-cache lock; production readers use try_lock.
  [[nodiscard]] bool hold_snapshot_lock_for_test() const noexcept {
    return mutex_.try_lock();
  }
  void release_snapshot_lock_for_test() const noexcept { mutex_.unlock(); }
#endif

private:
  [[nodiscard]] bool publish_cache() noexcept;

  mutable std::mutex mutex_{};
  // These are owned by the telemetry worker. The mutex protects only the
  // reader cache below, so a descheduled logger cannot block decoding or lose
  // a stale transition.
  DebugSnapshot current_{};
  DebugSnapshot stale_{};
  DebugSnapshot pending_{};
  bool pending_frame_{false};
  std::uint64_t frame_count_{0};
  std::uint64_t counter_gap_count_{0};
  std::uint64_t update_not_advanced_count_{0};
  bool previous_counter_initialized_{false};
  std::uint8_t last_counter_{0};
  bool last_frame_timestamp_initialized_{false};
  std::uint64_t last_frame_timestamp_us_{0};
  bool availability_initialized_{false};
  Availability previous_availability_{Availability::NoData};
  DebugSnapshot published_current_{};
  DebugSnapshot published_stale_{};
  mutable std::atomic<std::uint64_t> snapshot_read_drops_{0};
  mutable std::atomic<std::uint64_t> recorder_write_contention_{0};
};

} // namespace mazda::internal
