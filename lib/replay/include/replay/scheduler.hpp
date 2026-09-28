#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "controller_config/timing.hpp"
#include "mazda/facade_contracts.hpp"
#include "replay/controller.hpp"

namespace replay {

enum class ReplayScheduleStatus : std::uint8_t {
  Ok,
  InvalidInput,
  InvalidOptions,
  ClockFailure,
  ControllerFailure,
  // A ReplayGate refused the next due time; the controller was still stopped.
  Interrupted,
};

// Declaration order is not dispatch order: SignalSample is appended to keep
// the existing values stable, but at a shared timestamp it runs before
// OutputTick (see run_replay()).
enum class ReplayEventKind : std::uint8_t {
  Frame,
  EndOfStream,
  Timeout,
  Poll,
  OutputTick,
  SignalSample,
};

struct ReplayEvent {
  vehicle_core::MonotonicTimestamp time_us{0};
  ReplayEventKind kind{ReplayEventKind::Frame};
};

class ReplayEventSink {
public:
  virtual ~ReplayEventSink() = default;
  virtual void record(ReplayEvent event) noexcept = 0;
};

// Admits each replay-time step before the scheduler advances the replay clock
// to it. The scheduler itself never consults wall time: a host player can
// implement this to pace, pause, or cancel a replay without the pipeline
// seeing anything but replay-time units. Returning false interrupts the
// replay before any work at next_time_us is processed.
class ReplayGate {
public:
  virtual ~ReplayGate() = default;
  [[nodiscard]] virtual bool admit(vehicle_core::MonotonicTimestamp next_time_us) = 0;
};

struct ReplayScheduleOptions {
  // Inclusive replay horizon. Supply a tail beyond the last input timestamp
  // when freshness, polled rules, or animation must continue after EOF.
  vehicle_core::MonotonicTimestamp end_time_us{0};
  vehicle_core::Microseconds availability_period_us{mazda::kDefaultAvailabilityServiceTargetUs};
  // Output-stage cadence. Unset uses the stage's own tick_period_us().
  std::optional<vehicle_core::Microseconds> output_tick_period_us{};
  vehicle_core::Microseconds poll_period_us{controller_config::kPolledRuleSamplePeriodUs};
  // Polled-signal sampling cadence for SignalObservers. Unset uses
  // poll_period_us. Not scheduled without observers, but zero is rejected
  // with InvalidOptions either way.
  std::optional<vehicle_core::Microseconds> signal_sample_period_us{};
};

struct ReplayScheduleResult {
  ReplayScheduleStatus status{ReplayScheduleStatus::Ok};
  ReplayControllerStatus controller_status{ReplayControllerStatus::Ok};
  ReplayControllerStatus stop_status{ReplayControllerStatus::Ok};
  std::uint64_t frames_delivered{0};
  std::uint64_t timeout_publications{0};
  std::uint64_t polled_samples{0};
  std::uint64_t output_ticks{0};
  std::uint64_t signal_samples{0};
  bool end_of_stream{false};

  [[nodiscard]] constexpr bool ok() const noexcept { return status == ReplayScheduleStatus::Ok; }
};

// Drive the host controller in replay time, with no wall-clock pacing. The
// scheduler's event clock is deterministic, but each controller step still
// waits up to ReplayController's 500 ms wall-clock synchronization timeout for
// its worker-thread publication. A loaded host can therefore return
// SynchronizationTimeout for an otherwise identical input; callers should
// treat that status as host scheduling failure, not replay-time divergence.
// The input must be the normalized, ordered sequence from gvret::prepare_replay(). At a
// shared timestamp, all frames are delivered in source order, followed by
// EOF (once), a due availability timeout, a polled-rule sample, a polled
// signal sample (only with observers), and an output-stage tick. Poll,
// signal-sample and output ticks run at time zero; availability begins after
// its first period. Notified signals reach observers while the frame, EOF or
// timeout that changed them is processed, so they precede that timestamp's
// signal sample and output tick.
// Invalid input/options are rejected before the controller or stage is started.
// An optional gate is asked before every clock advance (including the final
// advance to the horizon); a refusal returns Interrupted after stopping the
// controller, so the stage still fails off.
[[nodiscard]] ReplayScheduleResult run_replay(std::vector<gvret::TimedCanFrame> frames,
                                              ReplayClock &clock, OutputStage &output,
                                              ReplayScheduleOptions options,
                                              ReplayEventSink *events = nullptr,
                                              ReplayGate *gate = nullptr);

// As above, with SignalObservers injected into the controller alongside the
// output stage. An empty list is identical to the overload above.
[[nodiscard]] ReplayScheduleResult
run_replay(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock, OutputStage &output,
           SignalObservers observers, ReplayScheduleOptions options,
           ReplayEventSink *events = nullptr, ReplayGate *gate = nullptr);

} // namespace replay
