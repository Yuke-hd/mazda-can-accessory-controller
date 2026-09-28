#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace replay {

// One of the supported playback rates: 0.25x, 0.5x, 1x, 2x, or 5x. A rate
// scales only how replay time maps to wall time; it never changes which
// frames the replay produces.
class PlaybackRate final {
public:
  [[nodiscard]] static PlaybackRate normal() noexcept;
  // Accepts exactly the canonical JSON number tokens "0.25", "0.5", "1", "2",
  // and "5" (what JSON.stringify produces for those values).
  [[nodiscard]] static std::optional<PlaybackRate> from_token(std::string_view token) noexcept;

  [[nodiscard]] std::string_view text() const noexcept;
  // Wall time needed to play replay_us of replay time, rounded up and
  // saturating at microseconds::max().
  [[nodiscard]] std::chrono::microseconds wall_span(std::uint64_t replay_us) const noexcept;
  // Replay time played during wall, rounded down and saturating; negative
  // wall spans play nothing.
  [[nodiscard]] std::uint64_t replay_span(std::chrono::microseconds wall) const noexcept;

  friend bool operator==(PlaybackRate left, PlaybackRate right) noexcept {
    return left.index_ == right.index_;
  }
  friend bool operator!=(PlaybackRate left, PlaybackRate right) noexcept {
    return !(left == right);
  }

private:
  explicit PlaybackRate(std::size_t index) noexcept : index_(index) {}
  std::size_t index_;
};

enum class PlaybackCommandKind : std::uint8_t { Play, Pause, Restart, SetRate };

struct PlaybackCommand final {
  PlaybackCommandKind kind{PlaybackCommandKind::Play};
  // Meaningful only for SetRate.
  PlaybackRate rate{PlaybackRate::normal()};
};

// Browser control messages are small flat JSON objects; anything longer is
// rejected before parsing.
inline constexpr std::size_t kMaxControlMessageBytes = 256;

// Either a command or a short machine-readable rejection reason:
// "too_long", "malformed", "unknown_type", "unknown_command", or
// "unsupported_rate".
struct PlaybackCommandParse final {
  std::optional<PlaybackCommand> command{};
  std::string_view rejection{};
};

// Parses one browser control message:
//   {"type":"control","command":"play"|"pause"|"restart"}
//   {"type":"control","command":"rate","rate":0.25|0.5|1|2|5}
// Keys may appear in any order. Duplicate or unknown keys, escapes, nested
// values, and trailing data are rejected as malformed.
[[nodiscard]] PlaybackCommandParse parse_playback_command(std::string_view text) noexcept;

} // namespace replay
