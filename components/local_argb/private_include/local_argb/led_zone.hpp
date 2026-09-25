#pragma once

#include <cstddef>
#include <cstdint>

#include "local_argb/local_argb.h"

namespace local_argb::internal {

// Strip-local zone model. A zone names a contiguous run of pixels in the
// renderer's PixelFrame and the direction in which a partial fill grows. It
// deliberately knows nothing about vehicle signals or action semantics;
// callers decide what a fill level means.

enum class FillDirection : std::uint8_t {
  // Lit pixels grow from `start` toward `start + length - 1`.
  StartToEnd,
  // Lit pixels grow from `start + length - 1` toward `start`.
  EndToStart,
  // Lit pixels grow symmetrically from the zone centre toward both edges.
  // An odd-length zone has one centre pixel (`start + length / 2`) and lights
  // 1, 3, 5, ... pixels. An even-length zone has a centre pair
  // (`start + length / 2 - 1` and `start + length / 2`) and lights 2, 4, 6, ...
  // pixels. A rounded lit count whose parity does not match the zone length is
  // rounded down by one more pixel so the fill always stays symmetric.
  CenterOut,
};

struct LedZone {
  std::size_t start{0};
  std::size_t length{0};
  FillDirection direction{FillDirection::StartToEnd};
};

enum class ZoneValidity : std::uint8_t {
  Valid,
  // length == 0; an empty zone is rejected rather than silently drawing
  // nothing so configuration mistakes are visible.
  EmptyZone,
  // The zone does not fit inside [0, kLedCount).
  OutOfRange,
  // The direction value is not one of the declared enumerators.
  UnknownDirection,
};

// A fill level in [0, 1] held as an exact ratio so rounding happens once, in
// fill_zone. Construction clamps: numerator > denominator is full, and a zero
// denominator is treated as empty (fail-off) rather than as a division fault.
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

[[nodiscard]] ZoneValidity validate_zone(const LedZone &zone) noexcept;

// Lights `fraction` of `zone` in `color`. The lit pixel count is
// floor(length * fraction), so a partial fill never over-reports and only a
// full fraction lights the whole zone; CenterOut then applies its parity rule
// above. Only lit pixels are written: unlit zone pixels and pixels outside the
// zone keep their current value, so callers compose onto a frame they cleared.
// An invalid zone writes nothing and returns its validate_zone() result.
ZoneValidity fill_zone(PixelFrame &frame, const LedZone &zone, FillFraction fraction,
                       Rgb color) noexcept;

} // namespace local_argb::internal
