#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "vehicle_profile_runtime/tables.hpp"

namespace {

using vehicle_profile_runtime::append_result;
using vehicle_profile_runtime::ByteOrder;
using vehicle_profile_runtime::convert_boolean;
using vehicle_profile_runtime::convert_number;
using vehicle_profile_runtime::DecodeFunction;
using vehicle_profile_runtime::DecodeResult;
using vehicle_profile_runtime::DispatchEntry;
using vehicle_profile_runtime::EnumMapping;
using vehicle_profile_runtime::ExtendedValue;
using vehicle_profile_runtime::extract_raw_value;
using vehicle_profile_runtime::map_enum;
using vehicle_profile_runtime::NumberConversion;
using vehicle_profile_runtime::RawRange;
using vehicle_profile_runtime::RawValue;
using vehicle_profile_runtime::signed_value;
using vehicle_signals::SignalId;
using vehicle_signals::SignalValue;

vehicle_core::RawCanFrame frame(const std::array<std::uint8_t, 8> &data, std::uint8_t dlc = 8) {
  vehicle_core::RawCanFrame result{};
  result.identifier = 0x321;
  result.dlc = dlc;
  result.data = data;
  return result;
}

void synthetic_helper_decoder(const vehicle_core::RawCanFrame &input, DecodeResult *results,
                              const std::size_t capacity, std::size_t &count) {
  RawValue raw{};
  float number = 0.0F;
  const NumberConversion number_conversion{RawValue{0, 8, false}, ExtendedValue{0.5L},
                                           ExtendedValue{1.0L}};
  const RawRange number_range{RawValue{0, 8, false}, RawValue{UINT8_MAX, 8, false}};
  if (extract_raw_value(input, 0, 8, ByteOrder::Intel, false, raw) &&
      convert_number(raw, number_conversion, number_range, nullptr, 0, number)) {
    (void)append_result(results, capacity, count,
                        DecodeResult::success(SignalId{1}, SignalValue::number(number)));
  } else {
    (void)append_result(results, capacity, count, DecodeResult::invalid(SignalId{1}));
  }

  bool boolean = false;
  const NumberConversion boolean_conversion{RawValue{0, 1, false}, ExtendedValue{1.0L},
                                            ExtendedValue{0.0L}};
  const RawRange boolean_range{RawValue{0, 1, false}, RawValue{1, 1, false}};
  if (extract_raw_value(input, 8, 1, ByteOrder::Intel, false, raw) &&
      convert_boolean(raw, boolean_conversion, boolean_range, nullptr, 0, boolean)) {
    (void)append_result(results, capacity, count,
                        DecodeResult::success(SignalId{2}, SignalValue::boolean(boolean)));
  } else {
    (void)append_result(results, capacity, count, DecodeResult::invalid(SignalId{2}));
  }
}

void synthetic_custom_decoder(const vehicle_core::RawCanFrame &input, DecodeResult *results,
                              const std::size_t capacity, std::size_t &count) {
  (void)append_result(
      results, capacity, count,
      DecodeResult::success(SignalId{3}, SignalValue::boolean(input.data[0] == 0xA5)));
}

static_assert(std::is_same_v<DecodeFunction, void (*)(const vehicle_core::RawCanFrame &,
                                                      DecodeResult *, std::size_t, std::size_t &)>);
static_assert(std::is_trivially_copyable_v<DecodeResult>);

} // namespace

TEST_CASE("Intel extraction preserves unsigned and signed widths through 64 bits") {
  const auto all_ones = frame({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
  RawValue raw{};

  REQUIRE(extract_raw_value(all_ones, 0, 64, ByteOrder::Intel, false, raw));
  CHECK(raw.bits == UINT64_MAX);
  CHECK(raw.bit_length == 64);
  CHECK_FALSE(raw.is_signed);

  REQUIRE(extract_raw_value(all_ones, 0, 64, ByteOrder::Intel, true, raw));
  std::int64_t signed_result = 0;
  REQUIRE(signed_value(raw, signed_result));
  CHECK(signed_result == -1);

  const auto minimum = frame({0, 0, 0, 0, 0, 0, 0, 0x80});
  REQUIRE(extract_raw_value(minimum, 0, 64, ByteOrder::Intel, true, raw));
  REQUIRE(signed_value(raw, signed_result));
  CHECK(signed_result == std::numeric_limits<std::int64_t>::min());

  const auto signed_12 = frame({0x00, 0x80, 0, 0, 0, 0, 0, 0});
  REQUIRE(extract_raw_value(signed_12, 4, 12, ByteOrder::Intel, true, raw));
  REQUIRE(signed_value(raw, signed_result));
  CHECK(signed_result == -2048);
}

TEST_CASE("Motorola extraction follows the DBC sawtooth walk across bytes") {
  const auto input = frame({0xD6, 0xB3, 0x81, 0x4D, 0xA0, 0x5C, 0x3E, 0x71});
  RawValue raw{};

  REQUIRE(extract_raw_value(input, 7, 16, ByteOrder::Motorola, false, raw));
  CHECK(raw.bits == 0xD6B3U);

  REQUIRE(extract_raw_value(input, 7, 64, ByteOrder::Motorola, false, raw));
  CHECK(raw.bits == UINT64_C(0xD6B3814DA05C3E71));

  REQUIRE(
      extract_raw_value(frame({0x80, 0, 0, 0, 0, 0, 0, 0}), 7, 64, ByteOrder::Motorola, true, raw));
  std::int64_t minimum = 0;
  REQUIRE(signed_value(raw, minimum));
  CHECK(minimum == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("short and malformed frames reject only the requested extraction") {
  RawValue output{17, 8, false};
  const auto short_frame = frame({0x2A, 0, 0, 0, 0, 0, 0, 0}, 1);
  REQUIRE(extract_raw_value(short_frame, 0, 8, ByteOrder::Intel, false, output));
  CHECK(output.bits == 0x2A);
  CHECK_FALSE(extract_raw_value(short_frame, 8, 1, ByteOrder::Intel, false, output));
  CHECK(output.bits == 0x2A);

  auto remote = short_frame;
  remote.remote_request = true;
  CHECK_FALSE(extract_raw_value(remote, 0, 1, ByteOrder::Intel, false, output));
  auto invalid_length = short_frame;
  invalid_length.dlc = 9;
  CHECK_FALSE(extract_raw_value(invalid_length, 0, 1, ByteOrder::Intel, false, output));
}

TEST_CASE("normalized conversions use exact raw bounds and pivot-centered affine results") {
  RawValue raw{30, 6, false};
  const RawRange permitted{RawValue{0, 6, false}, RawValue{63, 6, false}};
  const RawRange invalid{RawValue{30, 6, false}, RawValue{30, 6, false}};
  const NumberConversion conversion{RawValue{30, 6, false}, ExtendedValue{1.0L},
                                    ExtendedValue{29.5L}};
  float result = 0.0F;

  REQUIRE(convert_number(raw, conversion, permitted, nullptr, 0, result));
  CHECK(result == 29.5F); // (30 * 2 + 1) * 0.5 - 1, hand-calculated
  raw.bits = 31;
  REQUIRE(convert_number(raw, conversion, permitted, nullptr, 0, result));
  CHECK(result == 30.5F);
  raw.bits = 30;
  CHECK_FALSE(convert_number(raw, conversion, permitted, &invalid, 1, result));

  // Exact host projection of source range [4, 6] for source = 10 - 2 * raw
  // is raw [2, 3], even though the source scale is negative.
  const RawRange negative_scale_permitted{RawValue{0, 3, false}, RawValue{5, 3, false}};
  const RawRange negative_scale_invalid{RawValue{2, 3, false}, RawValue{3, 3, false}};
  const NumberConversion negative_scale_conversion{RawValue{2, 3, false}, ExtendedValue{-1.0L},
                                                   ExtendedValue{0.0L}};
  REQUIRE(convert_number(RawValue{1, 3, false}, negative_scale_conversion, negative_scale_permitted,
                         &negative_scale_invalid, 1, result));
  CHECK(result == 1.0F);
  CHECK_FALSE(convert_number(RawValue{2, 3, false}, negative_scale_conversion,
                             negative_scale_permitted, &negative_scale_invalid, 1, result));

  // The wide raw value is one above the inclusive invalid interval. A
  // double-precision comparison would round it down to that interval.
  raw = RawValue{UINT64_C(9007199254740993), 64, false};
  const RawValue pivot{UINT64_C(9007199254740992), 64, false};
  const RawRange wide_permitted{pivot, RawValue{UINT64_C(9007199254740994), 64, false}};
  const RawRange neighboring_invalid{pivot, pivot};
  const NumberConversion canceled{pivot, ExtendedValue{1.0L}, ExtendedValue{0.0L}};
  REQUIRE(convert_number(raw, canceled, wide_permitted, &neighboring_invalid, 1, result));
  CHECK(result == 1.0F);

  const RawRange excludes_neighbor{RawValue{0, 64, false}, pivot};
  CHECK_FALSE(convert_number(raw, canceled, excludes_neighbor, nullptr, 0, result));
  const RawRange includes_neighbor{raw, raw};
  CHECK_FALSE(convert_number(raw, canceled, wide_permitted, &includes_neighbor, 1, result));
}

TEST_CASE("pivot-centered conversion preserves 64-bit opposite-endpoint distance") {
  const RawValue signed_min{UINT64_C(0x8000000000000000), 64, true};
  const RawValue signed_max{UINT64_C(0x7fffffffffffffff), 64, true};
  const RawRange full_signed{signed_min, signed_max};
  const NumberConversion opposite_pivot{signed_min, ExtendedValue{1.0L}, ExtendedValue{0.0L}};
  float result = 0.0F;

  REQUIRE(convert_number(signed_max, opposite_pivot, full_signed, nullptr, 0, result));
  CHECK(result == std::ldexp(1.0F, 64));

  const NumberConversion wrong_pivot{RawValue{0, 8, true}, ExtendedValue{1.0L},
                                     ExtendedValue{0.0L}};
  CHECK_FALSE(convert_number(signed_max, wrong_pivot, full_signed, nullptr, 0, result));

  const RawRange reversed{signed_max, signed_min};
  CHECK_FALSE(convert_number(signed_max, opposite_pivot, reversed, nullptr, 0, result));
  const RawRange wrong_shape{RawValue{0, 8, false}, RawValue{255, 8, false}};
  CHECK_FALSE(convert_number(signed_max, opposite_pivot, wrong_shape, nullptr, 0, result));
}

TEST_CASE("non-integral affine cancellation and binary32 limits are checked") {
  const RawValue maximum{UINT64_MAX, 64, false};
  const RawValue middle{UINT64_C(0x8000000000000000), 64, false};
  const RawRange all_unsigned{RawValue{0, 64, false}, maximum};
  const NumberConversion half_scale{middle, ExtendedValue{0.5L},
                                    ExtendedValue{-4611686018427387904.0L}};
  float result = 0.0F;

  REQUIRE(convert_number(maximum, half_scale, all_unsigned, nullptr, 0, result));
  CHECK(result == -0.5F); // UINT64_MAX * 0.5 - 2^63

  const RawValue one{1, 1, false};
  const RawRange bit_range{RawValue{0, 1, false}, one};
  const long double maximum_float = std::numeric_limits<float>::max();
  const NumberConversion finite_max{RawValue{0, 1, false}, ExtendedValue{maximum_float},
                                    ExtendedValue{0.0L}};
  REQUIRE(convert_number(one, finite_max, bit_range, nullptr, 0, result));
  CHECK(result == std::numeric_limits<float>::max());

  const NumberConversion overflow{RawValue{0, 1, false}, ExtendedValue{maximum_float * 2.0L},
                                  ExtendedValue{0.0L}};
  CHECK_FALSE(convert_number(one, overflow, bit_range, nullptr, 0, result));

  const NumberConversion underflow{RawValue{0, 1, false},
                                   ExtendedValue{std::numeric_limits<long double>::denorm_min()},
                                   ExtendedValue{0.0L}};
  CHECK_FALSE(convert_number(one, underflow, bit_range, nullptr, 0, result));

  const NumberConversion nonfinite{RawValue{0, 1, false},
                                   ExtendedValue{std::numeric_limits<long double>::infinity()},
                                   ExtendedValue{0.0L}};
  CHECK_FALSE(convert_number(one, nonfinite, bit_range, nullptr, 0, result));
}

TEST_CASE("binary32 conversion rounds integer boundaries without signed overflow") {
  const RawRange integer_range{RawValue{0, 64, false}, RawValue{UINT64_C(16777219), 64, false}};
  const NumberConversion identity{RawValue{0, 64, false}, ExtendedValue{1.0L}, ExtendedValue{0.0L}};
  float result = 0.0F;

  REQUIRE(convert_number(RawValue{UINT64_C(16777217), 64, false}, identity, integer_range, nullptr,
                         0, result));
  CHECK(result == 16777216.0F);
  REQUIRE(convert_number(RawValue{UINT64_C(16777219), 64, false}, identity, integer_range, nullptr,
                         0, result));
  CHECK(result == 16777220.0F);
}

TEST_CASE("boolean and enum helpers reject invalid values without numeric enum coercion") {
  const RawValue one{1, 1, false};
  const RawRange bit_range{RawValue{0, 1, false}, one};
  const NumberConversion boolean_conversion{RawValue{0, 1, false}, ExtendedValue{1.0L},
                                            ExtendedValue{0.0L}};
  bool boolean = false;
  REQUIRE(convert_boolean(one, boolean_conversion, bit_range, nullptr, 0, boolean));
  CHECK(boolean);

  const NumberConversion non_boolean{RawValue{0, 1, false}, ExtendedValue{2.0L},
                                     ExtendedValue{0.0L}};
  CHECK_FALSE(convert_boolean(one, non_boolean, bit_range, nullptr, 0, boolean));

  const EnumMapping wide_choices[] = {{UINT64_MAX, 7}, {UINT64_MAX - 1U, 8}, {UINT64_C(0xff), 9}};
  SignalValue mapped{};
  const RawValue wide_min{UINT64_MAX - 1U, 64, false};
  const RawValue wide_max{UINT64_MAX, 64, false};
  const RawRange wide_range{wide_min, wide_max};
  REQUIRE(map_enum(wide_max, wide_range, nullptr, 0, wide_choices, 3, mapped));
  CHECK(mapped.as_enumeration() == 7);
  REQUIRE(map_enum(wide_min, wide_range, nullptr, 0, wide_choices, 3, mapped));
  CHECK(mapped.as_enumeration() == 8);
  const RawRange reserved_range{wide_min, wide_min};
  CHECK_FALSE(map_enum(wide_min, wide_range, &reserved_range, 1, wide_choices, 3, mapped));

  const RawValue signed_minus_one{0xff, 8, true};
  const RawRange signed_range{signed_minus_one, RawValue{0, 8, true}};
  REQUIRE(map_enum(signed_minus_one, signed_range, nullptr, 0, wide_choices + 2, 1, mapped));
  CHECK(mapped.as_enumeration() == 9);
  const RawValue unmapped{1, 64, false};
  const RawRange unmapped_range{RawValue{0, 64, false}, RawValue{1, 64, false}};
  CHECK_FALSE(map_enum(unmapped, unmapped_range, nullptr, 0, wide_choices, 3, mapped));
}

TEST_CASE("helper and custom decoders share the bounded stateless dispatch signature") {
  const DispatchEntry helper_entry{0x321, 2, synthetic_helper_decoder};
  const DispatchEntry custom_entry{0x322, 1, synthetic_custom_decoder};
  const DecodeFunction decoders[] = {helper_entry.decode, custom_entry.decode};
  const auto input = frame({0x0A, 0x01, 0, 0, 0, 0, 0, 0}, 2);

  DecodeResult results[2]{};
  std::size_t count = 0;
  decoders[0](input, results, 2, count);
  REQUIRE(count == 2);
  CHECK(results[0].id == SignalId{1});
  CHECK(results[0].valid);
  CHECK(results[0].value.as_number() == 6.0F);
  CHECK(results[1].id == SignalId{2});
  CHECK(results[1].valid);

  DecodeResult short_results[2]{};
  count = 0;
  decoders[0](frame({0x0A, 0x01, 0, 0, 0, 0, 0, 0}, 1), short_results, 2, count);
  REQUIRE(count == 2);
  CHECK(short_results[0].valid);
  CHECK_FALSE(short_results[1].valid);

  DecodeResult custom_results[1]{};
  count = 0;
  decoders[1](frame({0xA5, 0, 0, 0, 0, 0, 0, 0}, 1), custom_results, 1, count);
  REQUIRE(count == 1);
  CHECK(custom_results[0].valid);
  CHECK(custom_results[0].value.as_boolean() == true);

  DecodeResult bounded[1]{};
  count = 0;
  CHECK(append_result(bounded, 1, count, DecodeResult::invalid(SignalId{4})));
  CHECK_FALSE(append_result(bounded, 1, count, DecodeResult::invalid(SignalId{5})));
  CHECK(count == 1);
  CHECK(bounded[0].id == SignalId{4});
}
