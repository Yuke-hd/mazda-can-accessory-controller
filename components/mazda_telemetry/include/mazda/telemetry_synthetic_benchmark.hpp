#pragma once

#include "mazda/telemetry_profiling.hpp"

#include <cstdint>

namespace mazda::benchmark {

// Result of the opt-in ESP synthetic workload. The frame sequence is fixed
// in the component implementation and is delivered through Runtime's
// AcquisitionSource boundary; no vehicle data or CAN driver is involved.
struct SyntheticBenchmarkResult final {
  bool started{false};
  bool timed_out{false};
  std::uint64_t elapsed_us{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_processed{0};
  bool profile_ready{false};
  TelemetryProfileSnapshot profile{};
};

[[nodiscard]] SyntheticBenchmarkResult run_synthetic_benchmark() noexcept;

} // namespace mazda::benchmark
