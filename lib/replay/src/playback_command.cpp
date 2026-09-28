#include "replay/playback_command.hpp"

#include <array>
#include <limits>

namespace replay {

namespace {

struct RateEntry {
  std::string_view token;
  std::uint64_t numerator;
  std::uint64_t denominator;
};

constexpr std::array<RateEntry, 5> kRates{{
    {"0.25", 1, 4},
    {"0.5", 1, 2},
    {"1", 1, 1},
    {"2", 2, 1},
    {"5", 5, 1},
}};
constexpr std::size_t kNormalRate = 2;
constexpr std::uint64_t kMaxUnsigned = std::numeric_limits<std::uint64_t>::max();

// Computes value * multiplier / divisor, rounding up or down, saturating at
// kMaxUnsigned. Multipliers and divisors are the small rate terms above.
std::uint64_t scale(const std::uint64_t value, const std::uint64_t multiplier,
                    const std::uint64_t divisor, const bool round_up) noexcept {
  const auto quotient = value / divisor;
  const auto remainder = value % divisor;
  if (quotient > kMaxUnsigned / multiplier)
    return kMaxUnsigned;
  const auto whole = quotient * multiplier;
  const auto part = remainder * multiplier;
  const auto fraction = part / divisor + ((round_up && part % divisor != 0) ? 1U : 0U);
  if (whole > kMaxUnsigned - fraction)
    return kMaxUnsigned;
  return whole + fraction;
}

// A minimal reader for the flat control-message objects documented in the
// header. Values are either escape-free strings or JSON number tokens.
class ControlReader final {
public:
  explicit ControlReader(const std::string_view text) noexcept : text_(text) {}

  struct Fields {
    std::optional<std::string_view> type;
    std::optional<std::string_view> command;
    std::optional<std::string_view> rate;
  };

  [[nodiscard]] bool read(Fields &fields) noexcept {
    if (!consume('{'))
      return false;
    if (consume('}'))
      return at_end();
    do {
      if (!read_field(fields))
        return false;
    } while (consume(','));
    return consume('}') && at_end();
  }

private:
  [[nodiscard]] bool read_field(Fields &fields) noexcept {
    std::string_view key;
    if (!read_string(key) || !consume(':'))
      return false;
    if (key == "type")
      return read_string_once(fields.type);
    if (key == "command")
      return read_string_once(fields.command);
    if (key == "rate")
      return read_number_once(fields.rate);
    return false;
  }

  [[nodiscard]] bool read_string_once(std::optional<std::string_view> &field) noexcept {
    std::string_view value;
    if (field || !read_string(value))
      return false;
    field = value;
    return true;
  }

  [[nodiscard]] bool read_number_once(std::optional<std::string_view> &field) noexcept {
    std::string_view value;
    if (field || !read_number(value))
      return false;
    field = value;
    return true;
  }

  [[nodiscard]] bool read_string(std::string_view &value) noexcept {
    if (!consume('"'))
      return false;
    const auto start = position_;
    while (position_ < text_.size() && text_[position_] != '"') {
      const auto character = static_cast<unsigned char>(text_[position_]);
      if (character == '\\' || character < 0x20)
        return false;
      ++position_;
    }
    if (position_ == text_.size())
      return false;
    value = text_.substr(start, position_ - start);
    ++position_;
    return true;
  }

  // JSON number grammar: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
  [[nodiscard]] bool read_number(std::string_view &value) noexcept {
    skip_space();
    const auto start = position_;
    accept('-');
    if (!accept('0') && !digits())
      return false;
    if (accept('.') && !digits())
      return false;
    if (accept('e') || accept('E')) {
      if (!accept('+'))
        accept('-');
      if (!digits())
        return false;
    }
    value = text_.substr(start, position_ - start);
    return true;
  }

  [[nodiscard]] bool digits() noexcept {
    const auto start = position_;
    while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
      ++position_;
    return position_ != start;
  }

  bool accept(const char expected) noexcept {
    if (position_ >= text_.size() || text_[position_] != expected)
      return false;
    ++position_;
    return true;
  }

  [[nodiscard]] bool consume(const char expected) noexcept {
    skip_space();
    return accept(expected);
  }

  void skip_space() noexcept {
    while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                        text_[position_] == '\n' || text_[position_] == '\r'))
      ++position_;
  }

  [[nodiscard]] bool at_end() noexcept {
    skip_space();
    return position_ == text_.size();
  }

  std::string_view text_;
  std::size_t position_{0};
};

PlaybackCommandParse rejected(const std::string_view reason) noexcept { return {{}, reason}; }

std::optional<PlaybackCommandKind> simple_command(const std::string_view name) noexcept {
  if (name == "play")
    return PlaybackCommandKind::Play;
  if (name == "pause")
    return PlaybackCommandKind::Pause;
  if (name == "restart")
    return PlaybackCommandKind::Restart;
  return std::nullopt;
}

PlaybackCommandParse rate_command(const std::optional<std::string_view> token) noexcept {
  if (!token)
    return rejected("malformed");
  const auto rate = PlaybackRate::from_token(*token);
  if (!rate)
    return rejected("unsupported_rate");
  return {PlaybackCommand{PlaybackCommandKind::SetRate, *rate}, {}};
}

} // namespace

PlaybackRate PlaybackRate::normal() noexcept { return PlaybackRate{kNormalRate}; }

std::optional<PlaybackRate> PlaybackRate::from_token(const std::string_view token) noexcept {
  for (std::size_t index = 0; index < kRates.size(); ++index) {
    if (kRates[index].token == token)
      return PlaybackRate{index};
  }
  return std::nullopt;
}

std::string_view PlaybackRate::text() const noexcept { return kRates[index_].token; }

std::chrono::microseconds PlaybackRate::wall_span(const std::uint64_t replay_us) const noexcept {
  const auto &entry = kRates[index_];
  const auto wall = scale(replay_us, entry.denominator, entry.numerator, true);
  constexpr auto kMaxWall = static_cast<std::uint64_t>(std::chrono::microseconds::max().count());
  if (wall > kMaxWall)
    return std::chrono::microseconds::max();
  return std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(wall)};
}

std::uint64_t PlaybackRate::replay_span(const std::chrono::microseconds wall) const noexcept {
  if (wall.count() <= 0)
    return 0;
  const auto &entry = kRates[index_];
  return scale(static_cast<std::uint64_t>(wall.count()), entry.numerator, entry.denominator, false);
}

PlaybackCommandParse parse_playback_command(const std::string_view text) noexcept {
  if (text.size() > kMaxControlMessageBytes)
    return rejected("too_long");
  ControlReader::Fields fields{};
  if (!ControlReader{text}.read(fields))
    return rejected("malformed");
  if (fields.type != std::optional<std::string_view>{"control"})
    return rejected("unknown_type");
  if (!fields.command)
    return rejected("unknown_command");
  if (*fields.command == "rate")
    return rate_command(fields.rate);
  const auto kind = simple_command(*fields.command);
  if (!kind)
    return rejected("unknown_command");
  if (fields.rate)
    return rejected("malformed");
  return {PlaybackCommand{*kind, PlaybackRate::normal()}, {}};
}

} // namespace replay
