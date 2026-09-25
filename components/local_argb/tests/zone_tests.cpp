#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "local_argb/led_zone.hpp"
#include "local_argb/local_argb.h"

namespace {

using local_argb::kBlack;
using local_argb::kBlackFrame;
using local_argb::kLedCount;
using local_argb::PixelFrame;
using local_argb::Rgb;
using local_argb::internal::fill_zone;
using local_argb::internal::FillDirection;
using local_argb::internal::FillFraction;
using local_argb::internal::LedZone;
using local_argb::internal::validate_zone;
using local_argb::internal::ZoneValidity;

constexpr Rgb kColor{200, 100, 50};
// kColor scaled by integer maths: channel * numerator / denominator, floored.
constexpr Rgb kHalf{100, 50, 25};
constexpr Rgb kTenth{20, 10, 5};

using Pixel = std::pair<std::size_t, Rgb>;

// Asserts that exactly the listed pixels carry the listed colours and every
// other pixel of the frame is still black.
void assert_frame(const PixelFrame &frame, std::initializer_list<Pixel> pixels) {
  PixelFrame expected = kBlackFrame;
  for (const auto &pixel : pixels)
    expected[pixel.first] = pixel.second;
  assert(frame == expected);
}

PixelFrame filled(const LedZone zone, const FillFraction fraction) {
  PixelFrame frame = kBlackFrame;
  assert(fill_zone(frame, zone, fraction, kColor) == ZoneValidity::Valid);
  return frame;
}

void test_zone_can_cover_any_contiguous_subset_of_the_strip() {
  assert(validate_zone({0, kLedCount, FillDirection::StartToEnd}) == ZoneValidity::Valid);
  assert(validate_zone({0, 1, FillDirection::EndToStart}) == ZoneValidity::Valid);
  assert(validate_zone({kLedCount - 1, 1, FillDirection::CenterOut}) == ZoneValidity::Valid);
  assert(validate_zone({35, 30, FillDirection::CenterOut}) == ZoneValidity::Valid);

  const auto whole_strip = filled({0, kLedCount, FillDirection::StartToEnd}, FillFraction::full());
  PixelFrame all_lit{};
  all_lit.fill(kColor);
  assert(whole_strip == all_lit);
}

void test_invalid_zones_report_reason_and_leave_frame_untouched() {
  PixelFrame frame = kBlackFrame;
  frame[0] = Rgb{1, 2, 3};
  frame[kLedCount - 1] = Rgb{4, 5, 6};
  const PixelFrame before = frame;

  const LedZone empty{10, 0, FillDirection::StartToEnd};
  const LedZone start_past_end{kLedCount, 1, FillDirection::StartToEnd};
  const LedZone overruns_end{kLedCount - 2, 3, FillDirection::EndToStart};
  const LedZone wrapping_length{1, static_cast<std::size_t>(-1), FillDirection::CenterOut};
  const LedZone unknown_direction{0, 4, static_cast<FillDirection>(0x7f)};

  assert(validate_zone(empty) == ZoneValidity::EmptyZone);
  assert(validate_zone(start_past_end) == ZoneValidity::OutOfRange);
  assert(validate_zone(overruns_end) == ZoneValidity::OutOfRange);
  assert(validate_zone(wrapping_length) == ZoneValidity::OutOfRange);
  assert(validate_zone(unknown_direction) == ZoneValidity::UnknownDirection);

  for (const auto &zone :
       {empty, start_past_end, overruns_end, wrapping_length, unknown_direction}) {
    assert(fill_zone(frame, zone, FillFraction::full(), kColor) == validate_zone(zone));
    assert(frame == before);
  }
}

void test_fraction_clamps_and_treats_zero_denominator_as_empty() {
  assert(FillFraction::of(3, 2) == FillFraction::full());
  assert(FillFraction::of(0, 5) == FillFraction::empty());
  assert(FillFraction::of(7, 0) == FillFraction::empty());
  assert(FillFraction::of(2, 4).numerator() == 2);
  assert(FillFraction::of(2, 4).denominator() == 4);
}

void test_fraction_extremes_stay_exact() {
  assert(FillFraction::of(UINT32_MAX, UINT32_MAX) == FillFraction::full());

  // L = 100 * (MAX - 1) / MAX = 99 + (MAX - 100) / MAX: 99 full pixels and a
  // last pixel at (MAX - 100) / MAX, which floors every channel to c - 1.
  const auto frame = filled({0, kLedCount, FillDirection::StartToEnd},
                            FillFraction::of(UINT32_MAX - 1, UINT32_MAX));
  for (std::size_t index = 0; index + 1 < kLedCount; ++index)
    assert(frame[index] == kColor);
  assert((frame[kLedCount - 1] == Rgb{199, 99, 49}));
}

void test_start_to_end_fills_from_the_zone_start() {
  const LedZone zone{10, 5, FillDirection::StartToEnd};
  assert_frame(filled(zone, FillFraction::empty()), {});
  // L = 2.3: two full pixels, the third at 30%.
  assert_frame(filled(zone, FillFraction::of(23, 50)),
               {{10, kColor}, {11, kColor}, {12, Rgb{60, 30, 15}}});
  assert_frame(filled(zone, FillFraction::full()),
               {{10, kColor}, {11, kColor}, {12, kColor}, {13, kColor}, {14, kColor}});
}

void test_end_to_start_fills_from_the_zone_end() {
  const LedZone zone{10, 5, FillDirection::EndToStart};
  assert_frame(filled(zone, FillFraction::empty()), {});
  assert_frame(filled(zone, FillFraction::of(23, 50)),
               {{14, kColor}, {13, kColor}, {12, Rgb{60, 30, 15}}});
  assert_frame(filled(zone, FillFraction::full()),
               {{10, kColor}, {11, kColor}, {12, kColor}, {13, kColor}, {14, kColor}});
}

void test_fractional_remainder_lights_the_next_pixel_partially() {
  const LedZone zone{0, 4, FillDirection::StartToEnd};
  // L = 4/3: one full pixel, the next at 1/3.
  assert_frame(filled(zone, FillFraction::of(1, 3)), {{0, kColor}, {1, Rgb{66, 33, 16}}});
  // L = 8/3: two full pixels, the next at 2/3.
  assert_frame(filled(zone, FillFraction::of(2, 3)),
               {{0, kColor}, {1, kColor}, {2, Rgb{133, 66, 33}}});
  // L = 3.96: three full pixels, the last at 96%.
  assert_frame(filled(zone, FillFraction::of(99, 100)),
               {{0, kColor}, {1, kColor}, {2, kColor}, {3, Rgb{192, 96, 48}}});
  // L = 0.8: only a partial first pixel.
  assert_frame(filled(zone, FillFraction::of(1, 5)), {{0, Rgb{160, 80, 40}}});
}

void test_zero_remainder_writes_no_partial_pixel() {
  // L = 2 exactly: two full pixels and nothing after them.
  assert_frame(filled({0, 4, FillDirection::StartToEnd}, FillFraction::of(1, 2)),
               {{0, kColor}, {1, kColor}});
  assert_frame(filled({0, 4, FillDirection::EndToStart}, FillFraction::of(1, 2)),
               {{2, kColor}, {3, kColor}});
  assert_frame(filled({0, 4, FillDirection::CenterOut}, FillFraction::of(1, 2)),
               {{1, kColor}, {2, kColor}});
}

void test_center_out_odd_zone_lights_centre_first_then_both_sides() {
  // Odd zone of 5 at 20..24 with centre pixel 22.
  const LedZone zone{20, 5, FillDirection::CenterOut};
  assert_frame(filled(zone, FillFraction::empty()), {});
  // L = 0.5: the centre pixel alone, at half brightness.
  assert_frame(filled(zone, FillFraction::of(1, 10)), {{22, kHalf}});
  // L = 1: the full centre pixel.
  assert_frame(filled(zone, FillFraction::of(1, 5)), {{22, kColor}});
  // L = 2: full centre, each side gets (2 - 1) / 2 = 0.5.
  assert_frame(filled(zone, FillFraction::of(2, 5)), {{21, kHalf}, {22, kColor}, {23, kHalf}});
  assert_frame(filled(zone, FillFraction::of(3, 5)), {{21, kColor}, {22, kColor}, {23, kColor}});
  // L = 4: each side gets 1.5.
  assert_frame(filled(zone, FillFraction::of(4, 5)),
               {{20, kHalf}, {21, kColor}, {22, kColor}, {23, kColor}, {24, kHalf}});
  assert_frame(filled(zone, FillFraction::full()),
               {{20, kColor}, {21, kColor}, {22, kColor}, {23, kColor}, {24, kColor}});
}

void test_center_out_even_zone_splits_fill_between_halves() {
  // Even zone of 6 at 40..45 with centre pair 42/43.
  const LedZone zone{40, 6, FillDirection::CenterOut};
  // L = 1: each half gets 0.5.
  assert_frame(filled(zone, FillFraction::of(1, 6)), {{42, kHalf}, {43, kHalf}});
  assert_frame(filled(zone, FillFraction::of(2, 6)), {{42, kColor}, {43, kColor}});
  // L = 3: each half gets 1.5.
  assert_frame(filled(zone, FillFraction::of(3, 6)),
               {{41, kHalf}, {42, kColor}, {43, kColor}, {44, kHalf}});
  // L = 0.6: each half gets 0.3.
  assert_frame(filled(zone, FillFraction::of(1, 10)),
               {{42, Rgb{60, 30, 15}}, {43, Rgb{60, 30, 15}}});
  assert_frame(
      filled(zone, FillFraction::full()),
      {{40, kColor}, {41, kColor}, {42, kColor}, {43, kColor}, {44, kColor}, {45, kColor}});
}

void test_single_pixel_zone_shows_fraction_as_brightness_in_every_direction() {
  for (const auto direction :
       {FillDirection::StartToEnd, FillDirection::EndToStart, FillDirection::CenterOut}) {
    const LedZone zone{kLedCount - 1, 1, direction};
    assert_frame(filled(zone, FillFraction::of(1, 2)), {{kLedCount - 1, kHalf}});
    assert_frame(filled(zone, FillFraction::of(1, 10)), {{kLedCount - 1, kTenth}});
    assert_frame(filled(zone, FillFraction::full()), {{kLedCount - 1, kColor}});
  }
}

void test_fill_only_writes_lit_pixels_inside_the_zone() {
  PixelFrame frame{};
  const Rgb background{1, 1, 1};
  frame.fill(background);

  // L = 2.5: 53 and 52 full, 51 at half brightness.
  const LedZone zone{50, 4, FillDirection::EndToStart};
  assert(fill_zone(frame, zone, FillFraction::of(5, 8), kColor) == ZoneValidity::Valid);

  for (std::size_t index = 0; index < kLedCount; ++index) {
    if (index == 52 || index == 53)
      assert(frame[index] == kColor);
    else if (index == 51)
      assert(frame[index] == kHalf);
    else
      assert(frame[index] == background);
  }
}

void test_partial_pixel_that_scales_to_black_is_not_written() {
  PixelFrame frame{};
  const Rgb background{1, 1, 1};
  frame.fill(background);

  // 200 * 1 / 1000 floors to 0 on every channel.
  assert(fill_zone(frame, {7, 1, FillDirection::StartToEnd}, FillFraction::of(1, 1000), kColor) ==
         ZoneValidity::Valid);
  assert(frame[7] == background);
}

void test_same_zone_and_fraction_produce_identical_frames() {
  for (const auto direction :
       {FillDirection::StartToEnd, FillDirection::EndToStart, FillDirection::CenterOut}) {
    const LedZone zone{3, 17, direction};
    for (std::uint32_t numerator = 0; numerator <= 170; ++numerator) {
      const auto fraction = FillFraction::of(numerator, 170);
      assert(filled(zone, fraction) == filled(zone, fraction));
    }
  }
}

bool no_dimmer(const Rgb current, const Rgb previous) {
  return current.red >= previous.red && current.green >= previous.green &&
         current.blue >= previous.blue;
}

void test_pixels_never_dim_as_the_fraction_grows() {
  for (const auto direction :
       {FillDirection::StartToEnd, FillDirection::EndToStart, FillDirection::CenterOut}) {
    for (const std::size_t length : {std::size_t{1}, std::size_t{8}, std::size_t{9}}) {
      const LedZone zone{30, length, direction};
      PixelFrame previous = kBlackFrame;
      for (std::uint32_t numerator = 0; numerator <= 100; ++numerator) {
        const auto current = filled(zone, FillFraction::of(numerator, 100));
        for (std::size_t index = 0; index < kLedCount; ++index) {
          assert(no_dimmer(current[index], previous[index]));
          if (index < zone.start || index >= zone.start + zone.length)
            assert(current[index] == kBlack);
        }
        previous = current;
      }
    }
  }
}

} // namespace

int main() {
  test_zone_can_cover_any_contiguous_subset_of_the_strip();
  test_invalid_zones_report_reason_and_leave_frame_untouched();
  test_fraction_clamps_and_treats_zero_denominator_as_empty();
  test_fraction_extremes_stay_exact();
  test_start_to_end_fills_from_the_zone_start();
  test_end_to_start_fills_from_the_zone_end();
  test_fractional_remainder_lights_the_next_pixel_partially();
  test_zero_remainder_writes_no_partial_pixel();
  test_center_out_odd_zone_lights_centre_first_then_both_sides();
  test_center_out_even_zone_splits_fill_between_halves();
  test_single_pixel_zone_shows_fraction_as_brightness_in_every_direction();
  test_fill_only_writes_lit_pixels_inside_the_zone();
  test_partial_pixel_that_scales_to_black_is_not_written();
  test_same_zone_and_fraction_produce_identical_frames();
  test_pixels_never_dim_as_the_fraction_grows();
  return 0;
}
