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
  std::uint64_t work_budget_pauses{0};
  bool profile_ready{false};
  TelemetryProfileSnapshot profile{};
};

struct SyntheticBenchmarkProgress final {
  bool started{false};
  std::uint64_t work_budget_pauses{0};
  std::uint32_t requested_pause_ms{0};
};

[[nodiscard]] SyntheticBenchmarkResult run_synthetic_benchmark() noexcept;
[[nodiscard]] SyntheticBenchmarkProgress synthetic_benchmark_progress() noexcept;

} // namespace mazda::benchmark
