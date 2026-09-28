#pragma once

#include <chrono>
#include <cstdint>

#include "replay/playback_command.hpp"
#include "replay/scheduler.hpp"
#include "replay/wall_clock.hpp"

namespace replay {

enum class PlaybackPollStatus : std::uint8_t { Idle, Command, Closed };

struct PlaybackPoll final {
  PlaybackPollStatus status{PlaybackPollStatus::Idle};
  PlaybackCommand command{};
};

struct PlaybackState final {
  bool paused{false};
  PlaybackRate rate{PlaybackRate::normal()};
};

// The player's command source and state acknowledgement sink, for example a
// browser WebSocket. poll() must not block.
class PlaybackChannel {
public:
  virtual ~PlaybackChannel() = default;
  [[nodiscard]] virtual PlaybackPoll poll() = 0;
  virtual void report(PlaybackState state) = 0;
};

enum class PlaybackInterruption : std::uint8_t { None, Restart, Stop };

// The longest the pacer sleeps before checking for commands again.
inline constexpr std::chrono::microseconds kPlaybackCommandSlice{10'000};

// Maps replay time to wall time from an anchor: replay time anchor_replay
// plays at wall time anchor_wall, and later replay time follows at rate.
class PlaybackTimeline final {
public:
  PlaybackTimeline(std::chrono::microseconds anchor_wall,
                   vehicle_core::MonotonicTimestamp anchor_replay, PlaybackRate rate) noexcept
      : anchor_wall_(anchor_wall), anchor_replay_(anchor_replay), rate_(rate) {}

  [[nodiscard]] std::chrono::microseconds
  wall_target(vehicle_core::MonotonicTimestamp replay_time) const noexcept;
  [[nodiscard]] vehicle_core::MonotonicTimestamp
  position(std::chrono::microseconds wall_now) const noexcept;
  [[nodiscard]] vehicle_core::MonotonicTimestamp anchor_replay() const noexcept {
    return anchor_replay_;
  }
  [[nodiscard]] PlaybackRate rate() const noexcept { return rate_; }

private:
  std::chrono::microseconds anchor_wall_;
  vehicle_core::MonotonicTimestamp anchor_replay_;
  PlaybackRate rate_;
};

// Paces a replay against wall time and applies play, pause, rate, and restart
// commands. It is the only replay component that reads the wall clock: as a
// ReplayGate it holds each replay-time step until its wall-time equivalent,
// so while paused the scheduler processes no CAN frames, polled samples, or
// renderer ticks. Rate and pause affect only when steps are released, never
// which steps run, so every playback produces the same frame sequence.
class PlaybackPacer final : public ReplayGate {
public:
  PlaybackPacer(WallClock &wall, PlaybackChannel &channel) noexcept;

  // Starts (or restarts) playing from replay time zero, keeping the current
  // rate, and reports the state.
  void begin_replay();

  // Waits for next_time_us's wall-time equivalent while handling commands.
  // Returns false on restart or channel close; see interruption().
  [[nodiscard]] bool admit(vehicle_core::MonotonicTimestamp next_time_us) override;

  // After a replay ends, handles commands until restart (true) or channel
  // close (false).
  [[nodiscard]] bool await_restart();

  [[nodiscard]] PlaybackInterruption interruption() const noexcept { return interruption_; }

private:
  enum class Handling : std::uint8_t { Idle, Applied, Interrupted };

  Handling handle(PlaybackPoll poll, vehicle_core::MonotonicTimestamp due);
  void apply(PlaybackCommand command, vehicle_core::MonotonicTimestamp due);
  [[nodiscard]] bool drain(vehicle_core::MonotonicTimestamp due);
  void report();

  WallClock &wall_;
  PlaybackChannel &channel_;
  PlaybackTimeline timeline_;
  bool paused_{false};
  PlaybackInterruption interruption_{PlaybackInterruption::None};
};

} // namespace replay
