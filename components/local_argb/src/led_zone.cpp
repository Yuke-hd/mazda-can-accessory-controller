#include "../private_include/local_argb/led_zone.hpp"

namespace local_argb::internal {

namespace {

// Zone-relative run of lit pixels: offsets [first, first + count).
struct LitRun {
  std::size_t first{0};
  std::size_t count{0};
};

bool is_known_direction(const FillDirection direction) noexcept {
  switch (direction) {
  case FillDirection::StartToEnd:
  case FillDirection::EndToStart:
  case FillDirection::CenterOut:
    return true;
  }
  return false;
}

std::size_t rounded_down_lit_count(const std::size_t length, const FillFraction fraction) noexcept {
  // length <= kLedCount and numerator <= denominator, so the product fits and
  // the quotient never exceeds length.
  const auto scaled = static_cast<std::uint64_t>(length) * fraction.numerator();
  return static_cast<std::size_t>(scaled / fraction.denominator());
}

LitRun lit_run(const LedZone &zone, const FillFraction fraction) noexcept {
  auto count = rounded_down_lit_count(zone.length, fraction);
  switch (zone.direction) {
  case FillDirection::StartToEnd:
    return {0, count};
  case FillDirection::EndToStart:
    return {zone.length - count, count};
  case FillDirection::CenterOut:
    // Round down to the zone's parity so the run stays centred; zero stays
    // zero for an odd zone (see the header rule).
    if (count > 0 && count % 2 != zone.length % 2)
      --count;
    return {(zone.length - count) / 2, count};
  }
  return {};
}

} // namespace

ZoneValidity validate_zone(const LedZone &zone) noexcept {
  if (!is_known_direction(zone.direction))
    return ZoneValidity::UnknownDirection;
  if (zone.length == 0)
    return ZoneValidity::EmptyZone;
  // Written as a subtraction so a huge length cannot wrap start + length.
  if (zone.start >= kLedCount || zone.length > kLedCount - zone.start)
    return ZoneValidity::OutOfRange;
  return ZoneValidity::Valid;
}

ZoneValidity fill_zone(PixelFrame &frame, const LedZone &zone, const FillFraction fraction,
                       const Rgb color) noexcept {
  const auto validity = validate_zone(zone);
  if (validity != ZoneValidity::Valid)
    return validity;

  const auto run = lit_run(zone, fraction);
  for (std::size_t offset = 0; offset < run.count; ++offset)
    frame[zone.start + run.first + offset] = color;
  return ZoneValidity::Valid;
}

} // namespace local_argb::internal
