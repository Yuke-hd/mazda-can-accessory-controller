#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "gvret/replay_clock.hpp"
#include "gvret/replay_stream.hpp"
#include "local_argb/local_argb.h"

namespace gvret {

enum class ReplayControllerStatus : std::uint8_t {
  Ok,
  InvalidState,
  ConfigurationFailed,
  SynchronizationTimeout,
  TelemetryFault,
  RendererFault,
};

enum class ReplayInputResult : std::uint8_t { Frame, Timeout, EndOfStream, Fault };

struct ReplayStepResult final {
  ReplayControllerStatus status{ReplayControllerStatus::InvalidState};
  ReplayInputResult input{ReplayInputResult::Timeout};

  [[nodiscard]] constexpr bool ok() const noexcept { return status == ReplayControllerStatus::Ok; }
};

// Host composition of the production telemetry, policy and renderer path.
// The controller is one-shot: construct a new instance for each replay. Its
// synchronous methods are the scheduling boundary used by replay tooling;
// none advances the caller-owned clock. Call every lifecycle and operation
// method from the same host thread; the PixelFrameSink callback runs there.
class ReplayController final {
public:
  ReplayController(std::vector<TimedCanFrame> frames, ReplayClock &clock,
                   local_argb::PixelFrameSink &pixels,
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
  [[nodiscard]] ReplayControllerStatus render() noexcept;
  [[nodiscard]] ReplayControllerStatus stop() noexcept;

  [[nodiscard]] std::optional<vehicle_core::MonotonicTimestamp> next_frame_time() const noexcept;
  [[nodiscard]] bool running() const noexcept;

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace gvret
