#pragma once

#include <cstdint>

#include "mazda/facade_contracts.hpp"

namespace mazda {

enum class DebugSampleSource : std::uint8_t {
  None = 0,
  TelemetryObserverFrame = 1,
  TelemetryObserverTimeout = 2,
  TelemetryDueCheck = 3,
  LoggerPoll = 4,
  TwaiStatusPoll = 5,
};

// Fixed-size, opt-in evidence for the temporary CAN freshness investigation.
// The normal telemetry facade never emits diagnostics from this record and the
// firmware logger is the only production consumer. Values are zero/NoData when
// the diagnostic build flag is disabled.
struct DebugSnapshot final {
  bool enabled{false};
  bool has_frame{false};
  std::uint64_t frame_count{0};
  std::uint32_t identifier{0};
  std::uint8_t counter{0};
  std::uint8_t previous_counter{0};
  std::uint8_t expected_counter{0};
  bool counter_valid{false};
  bool counter_sequential{false};
  std::uint64_t counter_gap_count{0};

  bool has_previous_frame_timestamp{false};
  std::uint64_t previous_frame_timestamp_us{0};
  std::uint64_t frame_timestamp_us{0};
  std::uint64_t interarrival_us{0};
  std::uint64_t processing_timestamp_us{0};
  std::uint64_t rx_process_latency_us{0};
  std::uint64_t publication_timestamp_us{0};
  std::uint64_t process_publish_latency_us{0};

  bool signal_has_update{false};
  std::uint64_t signal_last_update_us{0};
  std::uint64_t now_us{0};
  std::uint64_t signal_age_us{0};
  bool has_freshness_timeout{false};
  std::uint64_t freshness_timeout_us{0};
  Availability availability{Availability::NoData};
  std::uint8_t process_status{0};
  // True when this processed 0x091 frame did not advance the signal's
  // last_update_us watermark. This is not proof that the decoder rejected it.
  bool update_not_advanced{false};
  std::uint64_t update_not_advanced_count{0};

  std::uint64_t esp_timer_us{0};
  std::uint64_t steady_clock_us{0};
  std::int64_t clock_delta_us{0};

  Diagnostics diagnostics{};
  std::uint64_t diagnostics_sample_timestamp_us{0};
  DebugSampleSource diagnostics_sample_source{DebugSampleSource::None};
  bool has_transport_last_frame{false};
  std::uint64_t transport_last_frame_age_us{0};
  std::uint64_t stale_transition_count{0};
  std::uint64_t recovery_count{0};
  // These counters measure bounded snapshot handoff contention. A nonzero
  // snapshot_read_drops value means the logger retried on a later poll; it
  // never blocks the telemetry worker or consumes a pending event.
  std::uint64_t snapshot_read_drops{0};
  std::uint64_t recorder_write_contention{0};
  std::uint64_t recorder_publications{0};
};

} // namespace mazda
