#include "gvret/replay_scheduler.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

namespace gvret {
namespace {

// Bounds accidental multi-year schedules with a tiny cadence before any
// controller or pixel output is touched.
constexpr std::uint64_t kMaximumScheduledEvents = 2'000'000;

[[nodiscard]] bool fits_event_budget(const std::size_t frame_count,
                                     const ReplayScheduleOptions options) noexcept {
  if (frame_count >= kMaximumScheduledEvents)
    return false;
  std::uint64_t remaining = kMaximumScheduledEvents - frame_count - 1; // EOF
  const auto fits = [&remaining](const std::uint64_t count) {
    if (count > remaining)
      return false;
    remaining -= count;
    return true;
  };
  const auto availability = options.end_time_us / options.availability_period_us;
  const auto renders = options.end_time_us / options.render_period_us;
  const auto polls = options.end_time_us / options.poll_period_us;
  return fits(availability) && renders < remaining && fits(renders + 1) && polls < remaining &&
         fits(polls + 1);
}

[[nodiscard]] ReplayScheduleStatus validate(const std::vector<TimedCanFrame> &frames,
                                            const ReplayClock &clock,
                                            const ReplayScheduleOptions options) noexcept {
  if (clock.now() != 0 || options.availability_period_us == 0 || options.render_period_us == 0 ||
      options.poll_period_us == 0)
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
  return fits_event_budget(frames.size(), options) ? ReplayScheduleStatus::Ok
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

ReplayScheduleResult run_replay(std::vector<TimedCanFrame> frames, ReplayClock &clock,
                                local_argb::PixelFrameSink &pixels,
                                const ReplayScheduleOptions options, ReplayEventSink *events) {
  ReplayScheduleResult result;
  result.status = validate(frames, clock, options);
  if (!result.ok())
    return result;

  ReplayController controller{std::move(frames), clock, pixels};
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
  std::optional<vehicle_core::MonotonicTimestamp> render{0};

  while (result.ok()) {
    auto due = controller.next_frame_time();
    include_earlier(due, availability);
    include_earlier(due, poll);
    include_earlier(due, render);
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
    if (render == due) {
      result.controller_status = controller.render();
      if (result.controller_status != ReplayControllerStatus::Ok) {
        result.status = ReplayScheduleStatus::ControllerFailure;
        break;
      }
      ++result.renderer_ticks;
      record(events, *due, ReplayEventKind::Render);
      render = next_deadline(*due, options.render_period_us, options.end_time_us);
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

} // namespace gvret
