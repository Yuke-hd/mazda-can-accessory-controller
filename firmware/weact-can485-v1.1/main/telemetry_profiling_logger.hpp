#pragma once

#include <cstdint>

#include "mazda/vehicle_telemetry.hpp"

namespace weact_can485::telemetry_profiling {

// Firmware-only, low-priority summary task for the opt-in bounded profiler.
// It never participates in acquisition, decoding, publication, lighting, or
// notification dispatch.
class Logger final {
public:
  explicit Logger(const mazda::VehicleTelemetry &telemetry) noexcept;

  [[nodiscard]] bool start() noexcept;

private:
  static void task_entry(void *context) noexcept;
  void run() noexcept;
  void poll(std::uint64_t sample_timestamp_us) noexcept;

  const mazda::VehicleTelemetry *telemetry_;
  std::uint64_t last_interval_end_us_{0};
};

} // namespace weact_can485::telemetry_profiling
