#pragma once

#include <cstddef>
#include <cstdint>

#include "local_argb/lighting_zone.hpp"
#include "local_argb/local_argb.h"

namespace local_argb::internal {

// Strip-local zone rendering. The zone and fill-level values live in the sink
// contract (local_argb/lighting_zone.hpp) so that a LightingCommand can carry
// them; this header adds validation and drawing. It deliberately knows nothing
// about vehicle signals or action semantics; callers decide what a fill level
// means.

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

[[nodiscard]] ZoneValidity validate_zone(const LedZone &zone) noexcept;

// Lights `fraction` of `zone` in `color`, treating the fraction as brightness.
// The lit amount L = length * fraction is kept as an exact rational. A run
// filled with amount A lights floor(A) pixels at full `color` in fill order,
// and the next pixel at the fractional remainder: e.g. L = 4.6 lights four
// full pixels and a fifth at 60%. A zero remainder adds no partial pixel.
// CenterOut splits L between its sides as described above.
//
// Partial brightness uses integer maths only: each channel becomes
// channel * remainder_numerator / remainder_denominator, rounded down, with
// every intermediate held in 64 bits for any uint32 fraction. A partial pixel
// that rounds to black is not written.
//
// Only lit and partial pixels are written: unlit zone pixels and pixels outside
// the zone keep their current value, so callers compose onto a frame they
// cleared. An invalid zone writes nothing and returns its validate_zone()
// result.
ZoneValidity fill_zone(PixelFrame &frame, const LedZone &zone, FillFraction fraction,
                       Rgb color) noexcept;

} // namespace local_argb::internal
