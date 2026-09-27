#pragma once

#include <cstdint>
#include <vector>

#include "gvret/replay_controller.hpp"

namespace gvret {

enum class ReplayScheduleStatus : std::uint8_t {
  Ok,
  InvalidInput,
  InvalidOptions,
  ClockFailure,
  ControllerFailure,
};

enum class ReplayEventKind : std::uint8_t { Frame, EndOfStream, Timeout, Poll, OutputTick };

struct ReplayEvent {
  vehicle_core::MonotonicTimestamp time_us{0};
  ReplayEventKind kind{ReplayEventKind::Frame};
};

class ReplayEventSink {
public:
  virtual ~ReplayEventSink() = default;
  virtual void record(ReplayEvent event) noexcept = 0;
};

struct ReplayScheduleOptions {
  // Inclusive replay horizon. Supply a tail beyond the last input timestamp
  // when freshness, polled rules, or animation must continue after EOF.
  vehicle_core::MonotonicTimestamp end_time_us{0};
  vehicle_core::Microseconds availability_period_us{10'000};
  // Output-stage cadence. It belongs to the local ARGB stage (its worker
  // pass), not to replay; the stage is currently RendererController (#92).
  vehicle_core::Microseconds output_tick_period_us{local_argb::kSupervisorPollUs};
  vehicle_core::Microseconds poll_period_us{100'000};
};

struct ReplayScheduleResult {
  ReplayScheduleStatus status{ReplayScheduleStatus::Ok};
  ReplayControllerStatus controller_status{ReplayControllerStatus::Ok};
  ReplayControllerStatus stop_status{ReplayControllerStatus::Ok};
  std::uint64_t frames_delivered{0};
  std::uint64_t timeout_publications{0};
  std::uint64_t polled_samples{0};
  std::uint64_t output_ticks{0};
  bool end_of_stream{false};

  [[nodiscard]] constexpr bool ok() const noexcept { return status == ReplayScheduleStatus::Ok; }
};

// Drive the host controller in replay time, with no wall-clock pacing. The
// input must be the normalized, ordered sequence from prepare_replay(). At a
// shared timestamp, all frames are delivered in source order, followed by
// EOF (once), a due availability timeout, a polled-rule sample, and an
// output-stage tick. Poll and output ticks run at time zero; availability
// begins after its first period.
// Invalid input/options are rejected before the controller or sink is started.
[[nodiscard]] ReplayScheduleResult run_replay(std::vector<TimedCanFrame> frames, ReplayClock &clock,
                                              local_argb::PixelFrameSink &pixels,
                                              ReplayScheduleOptions options,
                                              ReplayEventSink *events = nullptr);

} // namespace gvret
