#include "replay/playback_pacer.hpp"

#include <algorithm>
#include <limits>

namespace replay {

std::chrono::microseconds
PlaybackTimeline::wall_target(const vehicle_core::MonotonicTimestamp replay_time) const noexcept {
  if (replay_time <= anchor_replay_)
    return anchor_wall_;
  const auto span = rate_.wall_span(replay_time - anchor_replay_);
  if (span > std::chrono::microseconds::max() - anchor_wall_)
    return std::chrono::microseconds::max();
  return anchor_wall_ + span;
}

vehicle_core::MonotonicTimestamp
PlaybackTimeline::position(const std::chrono::microseconds wall_now) const noexcept {
  const auto played = rate_.replay_span(wall_now - anchor_wall_);
  if (played > std::numeric_limits<vehicle_core::MonotonicTimestamp>::max() - anchor_replay_)
    return std::numeric_limits<vehicle_core::MonotonicTimestamp>::max();
  return anchor_replay_ + played;
}

PlaybackPacer::PlaybackPacer(WallClock &wall, PlaybackChannel &channel) noexcept
    : wall_(wall), channel_(channel), timeline_(wall.now(), 0, PlaybackRate::normal()) {}

void PlaybackPacer::begin_replay() {
  paused_ = false;
  interruption_ = PlaybackInterruption::None;
  timeline_ = PlaybackTimeline{wall_.now(), 0, timeline_.rate()};
  report();
}

bool PlaybackPacer::admit(const vehicle_core::MonotonicTimestamp next_time_us) {
  for (;;) {
    if (!drain(next_time_us))
      return false;
    const auto now = wall_.now();
    if (paused_) {
      wall_.sleep_until(now + kPlaybackCommandSlice);
      continue;
    }
    const auto target = timeline_.wall_target(next_time_us);
    if (now >= target)
      return true;
    wall_.sleep_until(std::min(target, now + kPlaybackCommandSlice));
  }
}

bool PlaybackPacer::await_restart() {
  for (;;) {
    const auto handling = handle(channel_.poll(), timeline_.anchor_replay());
    if (handling == Handling::Interrupted)
      return interruption_ == PlaybackInterruption::Restart;
    if (handling == Handling::Idle)
      wall_.sleep_until(wall_.now() + kPlaybackCommandSlice);
  }
}

bool PlaybackPacer::drain(const vehicle_core::MonotonicTimestamp due) {
  for (;;) {
    const auto handling = handle(channel_.poll(), due);
    if (handling != Handling::Applied)
      return handling == Handling::Idle;
  }
}

PlaybackPacer::Handling PlaybackPacer::handle(const PlaybackPoll poll,
                                              const vehicle_core::MonotonicTimestamp due) {
  if (poll.status == PlaybackPollStatus::Idle)
    return Handling::Idle;
  if (poll.status == PlaybackPollStatus::Closed) {
    interruption_ = PlaybackInterruption::Stop;
    return Handling::Interrupted;
  }
  if (poll.command.kind == PlaybackCommandKind::Restart) {
    interruption_ = PlaybackInterruption::Restart;
    return Handling::Interrupted;
  }
  apply(poll.command, due);
  return Handling::Applied;
}

// Re-anchors the timeline at the current replay position so a pause, resume,
// or rate change never skips or repeats replay time. The position never
// passes the pending due time, which has not been released yet.
void PlaybackPacer::apply(const PlaybackCommand command,
                          const vehicle_core::MonotonicTimestamp due) {
  const auto now = wall_.now();
  const auto position =
      paused_ ? timeline_.anchor_replay() : std::min(due, timeline_.position(now));
  auto rate = timeline_.rate();
  if (command.kind == PlaybackCommandKind::SetRate)
    rate = command.rate;
  else
    paused_ = command.kind == PlaybackCommandKind::Pause;
  timeline_ = PlaybackTimeline{now, position, rate};
  report();
}

void PlaybackPacer::report() { channel_.report(PlaybackState{paused_, timeline_.rate()}); }

} // namespace replay
