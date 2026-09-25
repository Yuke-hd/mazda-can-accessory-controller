#include "../private_include/local_argb/led_zone.hpp"

namespace local_argb::internal {

namespace {

bool is_known_direction(const FillDirection direction) noexcept {
  switch (direction) {
  case FillDirection::StartToEnd:
  case FillDirection::EndToStart:
  case FillDirection::CenterOut:
    return true;
  }
  return false;
}

std::uint8_t scale_channel(const std::uint8_t channel, const std::uint64_t numerator,
                           const std::uint64_t denominator) noexcept {
  // channel < 2^8 and numerator < denominator < 2^33, so the product fits.
  return static_cast<std::uint8_t>(channel * numerator / denominator);
}

Rgb scale(const Rgb color, const std::uint64_t numerator,
          const std::uint64_t denominator) noexcept {
  return {scale_channel(color.red, numerator, denominator),
          scale_channel(color.green, numerator, denominator),
          scale_channel(color.blue, numerator, denominator)};
}

enum class Step : std::uint8_t { Forward, Backward };

// A run of pixels lit outward from `origin`, at most `capacity` pixels long.
// `amount / unit` is the exact number of pixels to light: its whole part is
// drawn at full colour and its remainder dims the next pixel in fill order.
struct Run {
  std::size_t origin{0};
  Step step{Step::Forward};
  std::size_t capacity{0};
  std::uint64_t amount{0};
  std::uint64_t unit{1};
};

void write_pixel(PixelFrame &frame, const std::size_t index, const Rgb color) noexcept {
  // A partial pixel that scales to black would erase the caller's frame
  // without lighting anything, so it is skipped like any unlit pixel.
  if (color != kBlack)
    frame[index] = color;
}

void light_run(PixelFrame &frame, const Run &run, const Rgb color) noexcept {
  const auto whole_pixels = run.amount / run.unit;
  const auto lit =
      whole_pixels < run.capacity ? static_cast<std::size_t>(whole_pixels) : run.capacity;
  const auto at = [&run](const std::size_t offset) {
    return run.step == Step::Forward ? run.origin + offset : run.origin - offset;
  };
  for (std::size_t offset = 0; offset < lit; ++offset)
    frame[at(offset)] = color;

  const auto remainder = run.amount % run.unit;
  if (remainder != 0 && lit < run.capacity)
    write_pixel(frame, at(lit), scale(color, remainder, run.unit));
}

void light_center_out(PixelFrame &frame, const LedZone &zone, const std::uint64_t lit_amount,
                      const std::uint64_t unit, const Rgb color) noexcept {
  const auto half = zone.length / 2;
  const auto centre = zone.start + half;
  std::uint64_t side_amount = lit_amount;
  std::size_t right_origin = centre;

  if (zone.length % 2 != 0) {
    // The single centre pixel takes the first min(L, 1) of the fill.
    if (lit_amount >= unit)
      frame[centre] = color;
    else if (lit_amount != 0)
      write_pixel(frame, centre, scale(color, lit_amount, unit));
    side_amount = lit_amount > unit ? lit_amount - unit : 0;
    right_origin = centre + 1;
  }

  // The left side always grows from centre - 1 (the left pixel of an even
  // zone's centre pair); the right side from the other centre pixel or the
  // pixel after an odd zone's centre.
  // Each side receives half of what is left: amount / (2 * unit) pixels.
  // half == 0 only for a one-pixel zone, whose sides have no capacity.
  if (half == 0 || side_amount == 0)
    return;
  const auto side_unit = 2 * unit;
  light_run(frame, {centre - 1, Step::Backward, half, side_amount, side_unit}, color);
  light_run(frame, {right_origin, Step::Forward, half, side_amount, side_unit}, color);
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

  // L = length * fraction pixels, kept exact as lit_amount / unit. length <=
  // kLedCount and the numerator is below 2^32, so the product fits in 64 bits.
  const auto lit_amount = static_cast<std::uint64_t>(zone.length) * fraction.numerator();
  const std::uint64_t unit = fraction.denominator();
  switch (zone.direction) {
  case FillDirection::StartToEnd:
    light_run(frame, {zone.start, Step::Forward, zone.length, lit_amount, unit}, color);
    break;
  case FillDirection::EndToStart:
    light_run(frame, {zone.start + zone.length - 1, Step::Backward, zone.length, lit_amount, unit},
              color);
    break;
  case FillDirection::CenterOut:
    light_center_out(frame, zone, lit_amount, unit, color);
    break;
  }
  return ZoneValidity::Valid;
}

} // namespace local_argb::internal
