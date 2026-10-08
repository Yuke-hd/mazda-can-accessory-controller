#include "mazda/telemetry_synthetic_benchmark.hpp"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mazda/definitions.hpp"
#include "mazda/vehicle_telemetry_internal.hpp"
#include "vehicle_telemetry/receive.hpp"
#include "vehicle_core/frame.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mazda::benchmark {
namespace {

constexpr std::size_t kSyntheticFrameCount = 4096;
constexpr std::size_t kSyntheticFrameKinds = 6;
constexpr std::uint64_t kWorkloadTimeoutUs = 10'000'000;
constexpr std::uint64_t kProfileWaitTimeoutUs = 7'000'000;
constexpr TickType_t kYieldTicks = pdMS_TO_TICKS(1) == 0 ? 1 : pdMS_TO_TICKS(1);

class BenchmarkClock final : public vehicle_core::MonotonicClock {
public:
  [[nodiscard]] vehicle_core::MonotonicTimestamp now() const noexcept override {
    return static_cast<vehicle_core::MonotonicTimestamp>(esp_timer_get_time());
  }
};

vehicle_core::RawCanFrame synthetic_frame(
    const std::size_t index, const vehicle_core::MonotonicTimestamp timestamp_us) noexcept {
  vehicle_core::RawCanFrame frame{};
  frame.timestamp_us = timestamp_us;
  frame.dlc = 8;
  switch (index % kSyntheticFrameKinds) {
  case 0:
    frame.identifier = candidate::kEngineDataId;
    frame.data = {0x09, 0x5b, 0, 0, 0, 0, 0, 0};
    break;
  case 1:
    frame.identifier = candidate::kGearId;
    frame.data = {0x24, 0x81, 0x07, 0xff, 0x04, 0xf0, 0, 0};
    break;
  case 2:
    frame.identifier = candidate::kTurnSwitchId;
    frame.data = {0, 0x20, 0x10, 0, 0, 0, 0, 0};
    break;
  case 3:
    frame.identifier = candidate::kBlinkInfoId;
    frame.data = {0, 0, 0x08, 0, 0x02, 0, 0, 0};
    break;
  case 4:
    frame.identifier = candidate::kDoorsId;
    frame.data = {0, 0, 0, 0x40, 0x3d, 0, 0, 0};
    break;
  case 5:
    frame.identifier = candidate::kAccelerationId;
    frame.data = {0x27, 0x11, 0x38, 0x80, 0, 0, 0, 0};
    break;
  default:
    break;
  }
  return frame;
}

class SyntheticSource final : public vehicle_telemetry::AcquisitionSource {
public:
  explicit SyntheticSource(vehicle_core::MonotonicClock &clock) noexcept : clock_(clock) {}

  [[nodiscard]] vehicle_telemetry::StatusResult start() noexcept override {
    next_index_.store(0, std::memory_order_relaxed);
    frames_received_.store(0, std::memory_order_relaxed);
    completion_timestamp_us_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::StatusResult stop() noexcept override {
    running_.store(false, std::memory_order_release);
    return {vehicle_telemetry::ResultCode::Ok};
  }

  [[nodiscard]] vehicle_telemetry::ReceiveStatus
  receive(vehicle_core::RawCanFrame &frame, const std::uint32_t) noexcept override {
    if (!running_.load(std::memory_order_acquire))
      return vehicle_telemetry::ReceiveStatus::NotStarted;

    const auto index = next_index_.fetch_add(1, std::memory_order_relaxed);
    if (index >= kSyntheticFrameCount) {
      auto completion_timestamp_us = std::uint64_t{0};
      completion_timestamp_us_.compare_exchange_strong(
          completion_timestamp_us, static_cast<std::uint64_t>(clock_.now()),
          std::memory_order_release, std::memory_order_relaxed);
      // Keep the Runtime worker alive until the benchmark owner observes the
      // complete count, while yielding so this finite source cannot consume
      // the target indefinitely after the sequence has ended.
      vTaskDelay(kYieldTicks);
      return vehicle_telemetry::ReceiveStatus::Timeout;
    }

    frame = synthetic_frame(index, clock_.now());
    frames_received_.fetch_add(1, std::memory_order_relaxed);
    return vehicle_telemetry::ReceiveStatus::Frame;
  }

  [[nodiscard]] vehicle_telemetry::AcquisitionStatistics statistics() const noexcept override {
    vehicle_telemetry::AcquisitionStatistics result{};
    result.frames_received = frames_received_.load(std::memory_order_relaxed);
    return result;
  }

  [[nodiscard]] std::uint64_t completion_timestamp_us() const noexcept {
    return completion_timestamp_us_.load(std::memory_order_acquire);
  }

private:
  vehicle_core::MonotonicClock &clock_;
  std::atomic<std::size_t> next_index_{0};
  std::atomic<std::uint64_t> frames_received_{0};
  std::atomic<std::uint64_t> completion_timestamp_us_{0};
  std::atomic<bool> running_{false};
};

} // namespace

SyntheticBenchmarkResult run_synthetic_benchmark() noexcept {
  SyntheticBenchmarkResult result{};
  // VehicleTelemetry owns a 32 KiB opaque service. Keep every object in
  // static storage because app_main has a small FreeRTOS stack. Declaration
  // order gives the service the earlier-lived clock, source, and sink.
  static BenchmarkClock clock{};
  static SyntheticSource source{clock};
  static internal::NullLightingSink lighting_sink{};
  static VehicleTelemetry telemetry{};
  internal::VehicleTelemetryAccess::emplace_benchmark_service(telemetry, clock, source,
                                                              lighting_sink);

  const auto started_us = clock.now();
  const auto start_result = telemetry.start();
  result.started = start_result.ok();
  if (!result.started)
    return result;

  const auto deadline_us = started_us + kWorkloadTimeoutUs;
  vehicle_core::MonotonicTimestamp workload_ended_us = started_us;
  for (;;) {
    const auto diagnostics = telemetry.diagnostics();
    const auto completion_timestamp_us = source.completion_timestamp_us();
    if (diagnostics.acquisition.frames_received >= kSyntheticFrameCount &&
        diagnostics.acquisition.frames_processed >= kSyntheticFrameCount &&
        completion_timestamp_us != 0) {
      // Runtime requests the next frame only after the final frame's
      // snapshot, observer, diagnostics, publication, notification, and
      // lighting callbacks have returned. The source stamps completion before
      // its post-sequence timeout delay, so this excludes that delay.
      workload_ended_us = completion_timestamp_us;
      break;
    }
    if (clock.now() >= deadline_us) {
      result.timed_out = true;
      workload_ended_us = clock.now();
      break;
    }
    vTaskDelay(kYieldTicks);
  }

  result.elapsed_us = workload_ended_us >= started_us ? workload_ended_us - started_us : 0;

#if defined(CONFIG_WEACT_CAN_TELEMETRY_PROFILING)
  if (!result.timed_out) {
    const auto profile_deadline = clock.now() + kProfileWaitTimeoutUs;
    for (;;) {
      result.profile = telemetry.telemetry_profile_snapshot();
      const auto &total = result.profile.stages[static_cast<std::size_t>(
          TelemetryProfileStage::Total)];
      if (result.profile.enabled && result.profile.interval_end_us != 0 &&
          total.calls > 0) {
        result.profile_ready = true;
        break;
      }
      if (clock.now() >= profile_deadline)
        break;
      vTaskDelay(kYieldTicks);
    }
  }
#endif

  (void)telemetry.stop();
  const auto diagnostics = telemetry.diagnostics();
  result.frames_received = diagnostics.acquisition.frames_received;
  result.frames_processed = diagnostics.acquisition.frames_processed;
  return result;
}

} // namespace mazda::benchmark
