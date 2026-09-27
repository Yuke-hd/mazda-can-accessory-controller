#include "replay/scheduler.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace replay {
namespace {

// Bounds generated cadence work with a tiny period before any controller or
// pixel output is touched. Input frames are already materialized by the
// caller, so they do not consume this schedule-event budget.
constexpr std::uint64_t kMaximumScheduledEvents = 2'000'000;

// The cadences a replay schedules, resolved from the options. A signal
// sample period is present only when observers are injected.
struct Cadences {
  vehicle_core::Microseconds output_tick_period_us{0};
  std::optional<vehicle_core::Microseconds> signal_sample_period_us{};
};

[[nodiscard]] bool fits_event_budget(const ReplayScheduleOptions options,
                                     const Cadences cadences) noexcept {
  std::uint64_t remaining = kMaximumScheduledEvents - 1; // EOF
  const auto fits = [&remaining](const std::uint64_t count) {
    if (count > remaining)
      return false;
    remaining -= count;
    return true;
  };
  const auto fits_inclusive_cadence = [&remaining](const std::uint64_t elapsed_ticks) {
    if (elapsed_ticks >= remaining)
      return false;
    remaining -= elapsed_ticks + 1;
    return true;
  };
  const auto availability = options.end_time_us / options.availability_period_us;
  const auto output_ticks = options.end_time_us / cadences.output_tick_period_us;
  const auto polls = options.end_time_us / options.poll_period_us;
  const auto signal_samples =
      cadences.signal_sample_period_us
          ? std::optional<std::uint64_t>{options.end_time_us / *cadences.signal_sample_period_us}
          : std::nullopt;
  return fits(availability) && fits_inclusive_cadence(output_ticks) &&
         fits_inclusive_cadence(polls) &&
         (!signal_samples || fits_inclusive_cadence(*signal_samples));
}

[[nodiscard]] ReplayScheduleStatus validate(const std::vector<gvret::TimedCanFrame> &frames,
                                            const ReplayClock &clock,
                                            const ReplayScheduleOptions options,
                                            const Cadences cadences) noexcept {
  if (clock.now() != 0 || options.availability_period_us == 0 ||
      cadences.output_tick_period_us == 0 || options.poll_period_us == 0 ||
      cadences.signal_sample_period_us == vehicle_core::Microseconds{0})
    return ReplayScheduleStatus::InvalidOptions;
  if (!frames.empty() && (frames.front().relative_time_us != 0 ||
                          frames.back().relative_time_us > options.end_time_us))
    return frames.front().relative_time_us != 0 ? ReplayScheduleStatus::InvalidInput
                                                : ReplayScheduleStatus::InvalidOptions;
  vehicle_core::MonotonicTimestamp previous = 0;
  for (const auto &timed : frames) {
    if (timed.relative_time_us < previous || timed.frame.timestamp_us != timed.relative_time_us)
      return ReplayScheduleStatus::InvalidInput;
    previous = timed.relative_time_us;
  }
  return fits_event_budget(options, cadences) ? ReplayScheduleStatus::Ok
                                              : ReplayScheduleStatus::InvalidOptions;
}

[[nodiscard]] std::optional<vehicle_core::MonotonicTimestamp>
next_deadline(const vehicle_core::MonotonicTimestamp current,
              const vehicle_core::Microseconds period,
              const vehicle_core::MonotonicTimestamp end) noexcept {
  if (period > std::numeric_limits<vehicle_core::MonotonicTimestamp>::max() - current)
    return std::nullopt;
  const auto next = current + period;
  return next <= end ? std::optional<vehicle_core::MonotonicTimestamp>{next} : std::nullopt;
}

void include_earlier(std::optional<vehicle_core::MonotonicTimestamp> &earliest,
                     const std::optional<vehicle_core::MonotonicTimestamp> candidate) noexcept {
  if (candidate && (!earliest || *candidate < *earliest))
    earliest = candidate;
}

void record(ReplayEventSink *events, const vehicle_core::MonotonicTimestamp time,
            const ReplayEventKind kind) noexcept {
  if (events != nullptr)
    events->record({time, kind});
}

} // namespace

ReplayScheduleResult run_replay(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock,
                                OutputStage &output, const ReplayScheduleOptions options,
                                ReplayEventSink *events) {
  return run_replay(std::move(frames), clock, output, SignalObservers{}, options, events);
}

ReplayScheduleResult run_replay(std::vector<gvret::TimedCanFrame> frames, ReplayClock &clock,
                                OutputStage &output, SignalObservers observers,
                                const ReplayScheduleOptions options, ReplayEventSink *events) {
  Cadences cadences{options.output_tick_period_us.value_or(output.tick_period_us())};
  if (!observers.empty())
    cadences.signal_sample_period_us =
        options.signal_sample_period_us.value_or(options.poll_period_us);
  const vehicle_core::Microseconds output_tick_period_us = cadences.output_tick_period_us;
  ReplayScheduleResult result;
  result.status = validate(frames, clock, options, cadences);
  if (!result.ok())
    return result;

  ReplayController controller{std::move(frames), clock, output, std::move(observers)};
  result.controller_status = controller.start();
  if (result.controller_status != ReplayControllerStatus::Ok) {
    result.status = ReplayScheduleStatus::ControllerFailure;
    return result;
  }

  std::optional<vehicle_core::MonotonicTimestamp> availability =
      options.availability_period_us <= options.end_time_us
          ? std::optional<vehicle_core::MonotonicTimestamp>{options.availability_period_us}
          : std::nullopt;
  std::optional<vehicle_core::MonotonicTimestamp> poll{0};
  std::optional<vehicle_core::MonotonicTimestamp> output_tick{0};
  std::optional<vehicle_core::MonotonicTimestamp> signal_sample =
      cadences.signal_sample_period_us ? std::optional<vehicle_core::MonotonicTimestamp>{0}
                                       : std::nullopt;

  while (result.ok()) {
    auto due = controller.next_frame_time();
    include_earlier(due, availability);
    include_earlier(due, poll);
    include_earlier(due, signal_sample);
    include_earlier(due, output_tick);
    if (!due)
      break;
    if (!clock.advance_to(*due)) {
      result.status = ReplayScheduleStatus::ClockFailure;
      break;
    }

    while (controller.next_frame_time() == due) {
      const auto step = controller.process_next_frame();
      if (step.status != ReplayControllerStatus::Ok || step.input != ReplayInputResult::Frame) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        result.controller_status = step.status == ReplayControllerStatus::Ok
                                       ? ReplayControllerStatus::TelemetryFault
                                       : step.status;
        break;
      }
      ++result.frames_delivered;
      record(events, *due, ReplayEventKind::Frame);
    }
    if (!result.ok())
      break;

    if (!result.end_of_stream && !controller.next_frame_time()) {
      const auto step = controller.process_next_frame();
      if (step.status != ReplayControllerStatus::Ok ||
          step.input != ReplayInputResult::EndOfStream) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        result.controller_status = step.status == ReplayControllerStatus::Ok
                                       ? ReplayControllerStatus::TelemetryFault
                                       : step.status;
        break;
      }
      result.end_of_stream = true;
      record(events, *due, ReplayEventKind::EndOfStream);
    }

    if (availability == due) {
      const auto step = controller.process_timeout();
      if (step.status != ReplayControllerStatus::Ok || step.input != ReplayInputResult::Timeout) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        result.controller_status = step.status == ReplayControllerStatus::Ok
                                       ? ReplayControllerStatus::TelemetryFault
                                       : step.status;
        break;
      }
      ++result.timeout_publications;
      record(events, *due, ReplayEventKind::Timeout);
      availability = next_deadline(*due, options.availability_period_us, options.end_time_us);
    }
    if (poll == due) {
      result.controller_status = controller.sample_polled_rules();
      if (result.controller_status != ReplayControllerStatus::Ok) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        break;
      }
      ++result.polled_samples;
      record(events, *due, ReplayEventKind::Poll);
      poll = next_deadline(*due, options.poll_period_us, options.end_time_us);
    }
    if (signal_sample == due) {
      result.controller_status = controller.sample_signals();
      if (result.controller_status != ReplayControllerStatus::Ok) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        break;
      }
      ++result.signal_samples;
      record(events, *due, ReplayEventKind::SignalSample);
      signal_sample = next_deadline(*due, *cadences.signal_sample_period_us, options.end_time_us);
    }
    if (output_tick == due) {
      result.controller_status = controller.tick_output();
      if (result.controller_status != ReplayControllerStatus::Ok) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        break;
      }
      ++result.output_ticks;
      record(events, *due, ReplayEventKind::OutputTick);
      output_tick = next_deadline(*due, output_tick_period_us, options.end_time_us);
    }
  }

  // The horizon is inclusive for scheduled work. Finish at the exact end
  // even when it falls between cadence ticks, before the final fail-off.
  if (result.ok() && !clock.advance_to(options.end_time_us))
    result.status = ReplayScheduleStatus::ClockFailure;

  result.stop_status = controller.stop();
  if (result.ok() && result.stop_status != ReplayControllerStatus::Ok) {
    result.status = ReplayScheduleStatus::ControllerFailure;
    result.controller_status = result.stop_status;
  }
  return result;
}

} // namespace replay
