#pragma once

#include <cstdint>

#include "freshness_debug_rate_limiter.hpp"
#include "mazda/vehicle_telemetry.hpp"

namespace weact_can485::freshness_debug {

// The logger is deliberately a firmware-only observer. It never participates
// in CAN acquisition, decoding, publication, or LED control.
class Logger final {
public:
  explicit Logger(const mazda::VehicleTelemetry &telemetry) noexcept;

  // Starts one low-priority task. A failed task allocation is reported to the
  // caller and must not gate the production telemetry path.
  [[nodiscard]] bool start() noexcept;

private:
  static void task_entry(void *context) noexcept;
  void run() noexcept;
  void poll(std::uint64_t sample_timestamp_us) noexcept;

  const mazda::VehicleTelemetry *telemetry_;
  std::uint64_t observed_stale_transition_count_{0};
  std::uint64_t stale_events_coalesced_{0};
  std::uint64_t stale_events_suppressed_{0};
  std::uint64_t snapshot_read_drops_{0};
  StaleLogRateLimiter stale_log_rate_limiter_{};
  std::uint64_t next_summary_timestamp_us_{0};
};

} // namespace weact_can485::freshness_debug
