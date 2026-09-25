#pragma once

#include <cstddef>
#include <cstdint>

namespace local_argb::internal {

// Strip-local zone values shared by the renderer and its value-only sink
// contract. A zone names a contiguous run of pixels in the renderer's
// PixelFrame and the direction in which a partial fill grows. These are plain
// values: validation and drawing stay in the renderer (led_zone.hpp).

enum class FillDirection : std::uint8_t {
  // Lit pixels grow from `start` toward `start + length - 1`.
  StartToEnd,
  // Lit pixels grow from `start + length - 1` toward `start`.
  EndToStart,
  // Lit pixels grow symmetrically from the zone centre toward both edges.
  // An even-length zone splits L (see fill_zone) evenly: each half receives
  // L / 2, filled outward from the centre pair (`start + length / 2 - 1` and
  // `start + length / 2`). An odd-length zone lights its centre pixel
  // (`start + length / 2`) at brightness min(L, 1) first; each side then
  // receives (L - 1) / 2, when positive, filled outward from the centre.
  // Both sides always carry the same brightness, so the fill stays symmetric.
  CenterOut,
};

struct LedZone {
  std::size_t start{0};
  std::size_t length{0};
  FillDirection direction{FillDirection::StartToEnd};
};

constexpr bool operator==(const LedZone &left, const LedZone &right) noexcept {
  return left.start == right.start && left.length == right.length &&
         left.direction == right.direction;
}
constexpr bool operator!=(const LedZone &left, const LedZone &right) noexcept {
  return !(left == right);
}

// A fill level in [0, 1] held as an exact ratio so no floating point is
// involved and rounding happens only when fill_zone scales a colour channel. Construction clamps:
// numerator > denominator is full, and a zero denominator is treated as empty (fail-off) rather
// than as a division fault.
class FillFraction {
public:
  [[nodiscard]] static constexpr FillFraction of(const std::uint32_t numerator,
                                                 const std::uint32_t denominator) noexcept {
    if (denominator == 0)
      return empty();
    return FillFraction{numerator > denominator ? denominator : numerator, denominator};
  }
  [[nodiscard]] static constexpr FillFraction empty() noexcept { return FillFraction{0, 1}; }
  [[nodiscard]] static constexpr FillFraction full() noexcept { return FillFraction{1, 1}; }

  [[nodiscard]] constexpr std::uint32_t numerator() const noexcept { return numerator_; }
  [[nodiscard]] constexpr std::uint32_t denominator() const noexcept { return denominator_; }

private:
  constexpr FillFraction(const std::uint32_t numerator, const std::uint32_t denominator) noexcept
      : numerator_(numerator), denominator_(denominator) {}

  std::uint32_t numerator_;
  std::uint32_t denominator_;
};

// Compares the represented values, so 1/2 == 2/4.
constexpr bool operator==(const FillFraction &left, const FillFraction &right) noexcept {
  return static_cast<std::uint64_t>(left.numerator()) * right.denominator() ==
         static_cast<std::uint64_t>(right.numerator()) * left.denominator();
}
constexpr bool operator!=(const FillFraction &left, const FillFraction &right) noexcept {
  return !(left == right);
}

} // namespace local_argb::internal
