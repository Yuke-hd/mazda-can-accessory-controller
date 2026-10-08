#pragma once

#include <cstdint>

namespace weact_can485::freertos_runtime_stats {

// Supplied by the firmware composition so the logger can correlate scheduler
// runtime with the generic Runtime's deliberate work-budget pauses. The core
// 0.2.1 interface exposes the pause count and request, but no completion
// timestamp for measuring the actual block duration.
struct PauseSnapshot final {
  bool available{false};
  std::uint64_t total_pauses{0};
  std::uint32_t requested_pause_ms{0};
  bool actual_blocked_available{false};
  std::uint64_t total_actual_blocked_us{0};
};

using PauseSnapshotReader = PauseSnapshot (*)(const void *context) noexcept;

class Logger final {
public:
  Logger(PauseSnapshotReader pause_reader, const void *pause_context) noexcept
      : pause_reader_(pause_reader), pause_context_(pause_context) {}

  [[nodiscard]] bool start() noexcept;

private:
  static void task_entry(void *context) noexcept;
  void run() noexcept;

  PauseSnapshotReader pause_reader_{nullptr};
  const void *pause_context_{nullptr};
};

} // namespace weact_can485::freertos_runtime_stats
