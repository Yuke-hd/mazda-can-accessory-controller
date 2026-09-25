#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

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

constexpr Rgb kColor{0, 12, 3};

// Asserts that exactly the listed indexes carry kColor and every other pixel
// of the frame is still black.
void assert_lit_exactly(const PixelFrame &frame, std::initializer_list<std::size_t> lit) {
  PixelFrame expected = kBlackFrame;
  for (const auto index : lit)
    expected[index] = kColor;
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

void test_start_to_end_fills_from_the_zone_start() {
  const LedZone zone{10, 5, FillDirection::StartToEnd};
  assert_lit_exactly(filled(zone, FillFraction::empty()), {});
  assert_lit_exactly(filled(zone, FillFraction::of(2, 5)), {10, 11});
  assert_lit_exactly(filled(zone, FillFraction::full()), {10, 11, 12, 13, 14});
}

void test_end_to_start_fills_from_the_zone_end() {
  const LedZone zone{10, 5, FillDirection::EndToStart};
  assert_lit_exactly(filled(zone, FillFraction::empty()), {});
  assert_lit_exactly(filled(zone, FillFraction::of(2, 5)), {13, 14});
  assert_lit_exactly(filled(zone, FillFraction::full()), {10, 11, 12, 13, 14});
}

void test_partial_fill_rounds_lit_count_down() {
  // 4 * 1/3 = 1.33 -> 1 pixel; 4 * 2/3 = 2.67 -> 2 pixels; 4 * 99/100 -> 3.
  const LedZone zone{0, 4, FillDirection::StartToEnd};
  assert_lit_exactly(filled(zone, FillFraction::of(1, 3)), {0});
  assert_lit_exactly(filled(zone, FillFraction::of(2, 3)), {0, 1});
  assert_lit_exactly(filled(zone, FillFraction::of(99, 100)), {0, 1, 2});
  assert_lit_exactly(filled(zone, FillFraction::of(1, 5)), {});
}

void test_center_out_odd_zone_grows_from_single_center_pixel() {
  // Odd zone of 5 at 20..24: centre pixel 22; lit counts are 1, 3, 5.
  const LedZone zone{20, 5, FillDirection::CenterOut};
  assert_lit_exactly(filled(zone, FillFraction::empty()), {});
  assert_lit_exactly(filled(zone, FillFraction::of(1, 5)), {22});
  // 2 of 5 is not symmetric; it rounds down to the single centre pixel.
  assert_lit_exactly(filled(zone, FillFraction::of(2, 5)), {22});
  assert_lit_exactly(filled(zone, FillFraction::of(3, 5)), {21, 22, 23});
  assert_lit_exactly(filled(zone, FillFraction::of(4, 5)), {21, 22, 23});
  assert_lit_exactly(filled(zone, FillFraction::full()), {20, 21, 22, 23, 24});
}

void test_center_out_even_zone_grows_from_two_center_pixels() {
  // Even zone of 6 at 40..45: centre pair 42/43; lit counts are 2, 4, 6.
  const LedZone zone{40, 6, FillDirection::CenterOut};
  // 1 of 6 cannot light a symmetric pair, so it stays dark.
  assert_lit_exactly(filled(zone, FillFraction::of(1, 6)), {});
  assert_lit_exactly(filled(zone, FillFraction::of(2, 6)), {42, 43});
  assert_lit_exactly(filled(zone, FillFraction::of(3, 6)), {42, 43});
  assert_lit_exactly(filled(zone, FillFraction::of(4, 6)), {41, 42, 43, 44});
  assert_lit_exactly(filled(zone, FillFraction::full()), {40, 41, 42, 43, 44, 45});
}

void test_single_pixel_zone_lights_only_when_full_in_every_direction() {
  for (const auto direction :
       {FillDirection::StartToEnd, FillDirection::EndToStart, FillDirection::CenterOut}) {
    const LedZone zone{kLedCount - 1, 1, direction};
    assert_lit_exactly(filled(zone, FillFraction::of(1, 2)), {});
    assert_lit_exactly(filled(zone, FillFraction::full()), {kLedCount - 1});
  }
}

void test_fill_only_writes_lit_pixels_inside_the_zone() {
  PixelFrame frame{};
  const Rgb background{1, 1, 1};
  frame.fill(background);

  const LedZone zone{50, 4, FillDirection::EndToStart};
  assert(fill_zone(frame, zone, FillFraction::of(1, 2), kColor) == ZoneValidity::Valid);

  for (std::size_t index = 0; index < kLedCount; ++index) {
    const bool lit = index == 52 || index == 53;
    assert(frame[index] == (lit ? kColor : background));
  }
}

void test_same_zone_and_fraction_produce_identical_frames() {
  for (const auto direction :
       {FillDirection::StartToEnd, FillDirection::EndToStart, FillDirection::CenterOut}) {
    const LedZone zone{3, 17, direction};
    for (std::uint32_t numerator = 0; numerator <= 17; ++numerator) {
      const auto fraction = FillFraction::of(numerator, 17);
      assert(filled(zone, fraction) == filled(zone, fraction));
    }
  }
}

void test_lit_pixels_never_shrink_as_the_fraction_grows() {
  for (const auto direction :
       {FillDirection::StartToEnd, FillDirection::EndToStart, FillDirection::CenterOut}) {
    for (const std::size_t length : {std::size_t{1}, std::size_t{8}, std::size_t{9}}) {
      const LedZone zone{30, length, direction};
      PixelFrame previous = kBlackFrame;
      for (std::uint32_t numerator = 0; numerator <= 100; ++numerator) {
        const auto current = filled(zone, FillFraction::of(numerator, 100));
        for (std::size_t index = 0; index < kLedCount; ++index) {
          if (previous[index] != kBlack)
            assert(current[index] == kColor);
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
  test_start_to_end_fills_from_the_zone_start();
  test_end_to_start_fills_from_the_zone_end();
  test_partial_fill_rounds_lit_count_down();
  test_center_out_odd_zone_grows_from_single_center_pixel();
  test_center_out_even_zone_grows_from_two_center_pixels();
  test_single_pixel_zone_lights_only_when_full_in_every_direction();
  test_fill_only_writes_lit_pixels_inside_the_zone();
  test_same_zone_and_fraction_produce_identical_frames();
  test_lit_pixels_never_shrink_as_the_fraction_grows();
  return 0;
}
