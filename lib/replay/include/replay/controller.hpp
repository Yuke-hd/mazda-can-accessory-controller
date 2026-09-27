#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "gvret/replay_stream.hpp"
#include "replay/clock.hpp"
#include "replay/output_stage.hpp"

namespace replay {

enum class ReplayControllerStatus : std::uint8_t {
  Ok,
  InvalidState,
  ConfigurationFailed,
  SynchronizationTimeout,
  TelemetryFault,
  OutputFault,
};

enum class ReplayInputResult : std::uint8_t { Frame, Timeout, EndOfStream, Fault };

struct ReplayStepResult final {
  ReplayControllerStatus status{ReplayControllerStatus::InvalidState};
  ReplayInputResult input{ReplayInputResult::Timeout};

  [[nodiscard]] constexpr bool ok() const noexcept { return status == ReplayControllerStatus::Ok; }
};

// Host composition of the production telemetry and policy path, driving an
// injected OutputStage. The controller is one-shot: construct a new instance
// for each replay; the caller-owned stage must outlive it. Its synchronous
// methods are the scheduling boundary used by replay tooling; none advances
// the caller-owned clock. Call every lifecycle and operation method from the
// same host thread; every OutputStage call runs there.
class ReplayController final {
public:
  ReplayController(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock,
                   OutputStage &output,
                   vehicle_core::Microseconds synchronization_timeout_us = 500'000);
  ~ReplayController() noexcept;

  ReplayController(const ReplayController &) = delete;
  ReplayController &operator=(const ReplayController &) = delete;
  ReplayController(ReplayController &&) = delete;
  ReplayController &operator=(ReplayController &&) = delete;

  [[nodiscard]] ReplayControllerStatus start() noexcept;
  [[nodiscard]] ReplayStepResult process_next_frame() noexcept;
  [[nodiscard]] ReplayStepResult process_timeout() noexcept;
  // Fault injection is a host lifecycle seam used to prove that a terminal
  // Runtime diagnostic is published before the worker exits.
  [[nodiscard]] ReplayStepResult process_source_fault() noexcept;
  [[nodiscard]] ReplayControllerStatus sample_polled_rules() noexcept;
  // One output-stage tick at the current replay time.
  [[nodiscard]] ReplayControllerStatus tick_output() noexcept;
  [[nodiscard]] ReplayControllerStatus stop() noexcept;

  [[nodiscard]] std::optional<vehicle_core::MonotonicTimestamp> next_frame_time() const noexcept;
  [[nodiscard]] bool running() const noexcept;

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace replay
