#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "companion_protocol/live_signals.hpp"

// Live signals notification pacing (docs/specs/companion/live-signals.md#notifications):
// the streaming conditions, the 100 ms rate cap, change detection against the
// last frame the BLE stack accepted, the 1 s heartbeat and the wrapping
// sequence. The BLE binding feeds it link events and samples; it never
// touches the BLE stack or the provider itself.

namespace companion_protocol {

// Monotonic time in milliseconds, from any fixed origin.
using LiveStreamClock = std::chrono::milliseconds;

// Consecutive samples, and so consecutive frames, are at least this far apart.
inline constexpr LiveStreamClock kLiveFrameInterval{100};
// A frame goes out when this long has passed since the last frame attempt.
inline constexpr LiveStreamClock kLiveHeartbeatInterval{1000};
// Frames are sent only on a link with at least this ATT MTU.
inline constexpr std::uint16_t kMinLiveSignalsAttMtu = 64;
static_assert(kLiveFrameBytes <= kMinLiveSignalsAttMtu - 3,
              "Live signals must fit one notification at the minimum ATT MTU");

// Port to the BLE stack: queue one notification without waiting. Returns
// whether the stack accepted the frame; a refused frame is dropped.
class LiveFrameSink {
public:
  virtual bool try_queue(const LiveFrame &frame) noexcept = 0;

protected:
  LiveFrameSink() noexcept = default;
  LiveFrameSink(const LiveFrameSink &) = default;
  LiveFrameSink &operator=(const LiveFrameSink &) = default;
  ~LiveFrameSink() = default;
};

// Paces the live signals stream of one connection. Not thread-safe: the BLE
// binding calls it from one context.
class LiveSignalStream final {
public:
  // The central enabled (true) or disabled notifications in the CCCD,
  // including a bonded peer's setting restored at re-encryption.
  void set_notifications_enabled(bool enabled) noexcept;
  // The link is encrypted with an accepted, stored bond.
  void set_link_secured(bool secured) noexcept;
  void set_att_mtu(std::uint16_t mtu) noexcept;
  // A connection opened or closed: every condition is cleared and the rate
  // history forgotten.
  void reset_connection() noexcept;

  // Whether every streaming condition holds.
  [[nodiscard]] bool streaming() const noexcept;

  // The wait until the next sample may be taken: zero when one is due,
  // std::nullopt while not streaming. Samples are at least
  // kLiveFrameInterval apart.
  [[nodiscard]] std::optional<LiveStreamClock>
  time_until_sample(LiveStreamClock now) const noexcept;

  // Offers one sampling pass taken at `now`. When a sample is due, sends a
  // frame through `sink` if this is the first frame since streaming
  // (re)started, the content differs from the last frame the stack accepted,
  // or kLiveHeartbeatInterval has passed since the last attempt. Returns
  // whether a frame was attempted; an offer that is not due changes nothing.
  bool offer(LiveStreamClock now, const LiveSignalContent &content, LiveFrameSink &sink) noexcept;

private:
  struct Conditions {
    bool notifications_enabled{false};
    bool link_secured{false};
    std::uint16_t att_mtu{0};

    [[nodiscard]] bool hold() const noexcept;
  };

  struct Pacing {
    std::uint8_t next_sequence{0};
    std::optional<LiveSignalContent> accepted{};
    std::optional<LiveStreamClock> last_sample{};
    std::optional<LiveStreamClock> last_attempt{};

    void restart() noexcept;
    [[nodiscard]] bool sample_due(LiveStreamClock now) const noexcept;
    [[nodiscard]] bool frame_due(LiveStreamClock now,
                                 const LiveSignalContent &content) const noexcept;
  };

  template <typename Update> void update_conditions(Update update) noexcept;

  Conditions conditions_{};
  Pacing pacing_{};
};

} // namespace companion_protocol
