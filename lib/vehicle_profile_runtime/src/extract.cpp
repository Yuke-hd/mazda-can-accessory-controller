#include "vehicle_profile_runtime/tables.hpp"

#include <cmath>
#include <limits>

namespace vehicle_profile_runtime {
namespace {

constexpr std::uint8_t kPayloadBits =
    static_cast<std::uint8_t>(vehicle_core::kCanClassicPayloadBytes * 8U);

[[nodiscard]] constexpr std::uint64_t width_mask(const std::uint8_t width) noexcept {
  return width == 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1U);
}

[[nodiscard]] bool valid_raw(const RawValue &raw) noexcept {
  return raw.bit_length > 0 && raw.bit_length <= 64;
}

[[nodiscard]] bool same_raw_shape(const RawValue &left, const RawValue &right) noexcept {
  return valid_raw(left) && left.bit_length == right.bit_length &&
         left.is_signed == right.is_signed && (left.bits & ~width_mask(left.bit_length)) == 0 &&
         (right.bits & ~width_mask(right.bit_length)) == 0;
}

struct SignedMagnitude final {
  std::uint64_t magnitude{0};
  bool negative{false};
};

[[nodiscard]] bool magnitude_of(const RawValue &raw, SignedMagnitude &output) noexcept {
  if (!valid_raw(raw) || (raw.bits & ~width_mask(raw.bit_length)) != 0) {
    return false;
  }
  const std::uint64_t bits = raw.bits;
  if (!raw.is_signed) {
    output = {bits, false};
    return true;
  }
  const std::uint64_t sign_bit = UINT64_C(1) << (raw.bit_length - 1U);
  if ((bits & sign_bit) == 0) {
    output = {bits, false};
    return true;
  }
  output = {((~bits) & width_mask(raw.bit_length)) + 1U, true};
  return true;
}

[[nodiscard]] int compare(const SignedMagnitude &left, const SignedMagnitude &right) noexcept {
  if (left.negative != right.negative) {
    return left.negative ? -1 : 1;
  }
  if (left.magnitude == right.magnitude) {
    return 0;
  }
  const bool left_is_less = left.magnitude < right.magnitude;
  return left.negative ? (left_is_less ? 1 : -1) : (left_is_less ? -1 : 1);
}

[[nodiscard]] bool compare_raw(const RawValue &left, const RawValue &right,
                               int &ordering) noexcept {
  if (!same_raw_shape(left, right)) {
    return false;
  }
  SignedMagnitude left_value{};
  SignedMagnitude right_value{};
  if (!magnitude_of(left, left_value) || !magnitude_of(right, right_value)) {
    return false;
  }
  ordering = compare(left_value, right_value);
  return true;
}

void two_sum(const long double left, const long double right, long double &sum,
             long double &error) noexcept {
  sum = left + right;
  const long double right_virtual = sum - left;
  error = (left - (sum - right_virtual)) + (right - right_virtual);
}

[[nodiscard]] ExtendedValue normalize(const long double high, const long double low) noexcept {
  long double sum = 0.0L;
  long double error = 0.0L;
  two_sum(high, low, sum, error);
  return {sum, error};
}

[[nodiscard]] ExtendedValue add(const ExtendedValue &left, const ExtendedValue &right) noexcept {
  long double sum = 0.0L;
  long double error = 0.0L;
  two_sum(left.high, right.high, sum, error);
  error += left.low;
  error += right.low;
  return normalize(sum, error);
}

[[nodiscard]] ExtendedValue negate(const ExtendedValue &value) noexcept {
  return {-value.high, -value.low};
}

[[nodiscard]] ExtendedValue multiply(const ExtendedValue &left,
                                     const ExtendedValue &right) noexcept {
  const long double product = left.high * right.high;
  long double error = std::fma(left.high, right.high, -product);
  error += left.high * right.low;
  error += left.low * right.high;
  error += left.low * right.low;
  return normalize(product, error);
}

[[nodiscard]] bool finite(const ExtendedValue &value) noexcept {
  return std::isfinite(value.high) && std::isfinite(value.low);
}

[[nodiscard]] int compare(const ExtendedValue &left, const ExtendedValue &right) noexcept {
  const ExtendedValue difference = add(left, negate(right));
  if (difference.high < 0.0L || (difference.high == 0.0L && difference.low < 0.0L)) {
    return -1;
  }
  if (difference.high > 0.0L || (difference.high == 0.0L && difference.low > 0.0L)) {
    return 1;
  }
  return 0;
}

[[nodiscard]] bool same_range_shape(const RawValue &raw, const RawRange &range) noexcept {
  return same_raw_shape(raw, range.minimum) && same_raw_shape(raw, range.maximum);
}

[[nodiscard]] bool in_range(const RawValue &raw, const RawRange &range,
                            bool &valid_range) noexcept {
  int range_order = 0;
  int lower_order = 0;
  int upper_order = 0;
  if (!same_range_shape(raw, range) || !compare_raw(range.minimum, range.maximum, range_order) ||
      range_order > 0 || !compare_raw(raw, range.minimum, lower_order) ||
      !compare_raw(raw, range.maximum, upper_order)) {
    valid_range = false;
    return false;
  }
  valid_range = true;
  return lower_order >= 0 && upper_order <= 0;
}

[[nodiscard]] bool raw_delta(const RawValue &raw, const RawValue &pivot,
                             ExtendedValue &output) noexcept {
  if (!same_raw_shape(raw, pivot)) {
    return false;
  }
  SignedMagnitude raw_value{};
  SignedMagnitude pivot_value{};
  if (!magnitude_of(raw, raw_value) || !magnitude_of(pivot, pivot_value)) {
    return false;
  }

  const int ordering = compare(raw_value, pivot_value);
  std::uint64_t magnitude = 0;
  bool negative = false;
  if (raw_value.negative == pivot_value.negative) {
    if (raw_value.negative) {
      magnitude = ordering >= 0 ? pivot_value.magnitude - raw_value.magnitude
                                : raw_value.magnitude - pivot_value.magnitude;
    } else {
      magnitude = ordering >= 0 ? raw_value.magnitude - pivot_value.magnitude
                                : pivot_value.magnitude - raw_value.magnitude;
    }
    negative = ordering < 0;
  } else {
    if (raw_value.magnitude > UINT64_MAX - pivot_value.magnitude) {
      return false;
    }
    magnitude = raw_value.magnitude + pivot_value.magnitude;
    negative = raw_value.negative;
  }

  constexpr long double kLowWordScale = 4294967296.0L;
  long double high = static_cast<long double>(magnitude >> 32U) * kLowWordScale;
  long double low = static_cast<long double>(magnitude & UINT64_C(0xffffffff));
  if (negative) {
    high = -high;
    low = -low;
  }
  output = {high, low};
  return true;
}

} // namespace

bool extract_raw_value(const vehicle_core::RawCanFrame &frame, const std::uint8_t start_bit,
                       const std::uint8_t bit_length, const ByteOrder byte_order,
                       const bool is_signed, RawValue &output) noexcept {
  if (frame.dlc > vehicle_core::kCanClassicPayloadBytes || frame.remote_request ||
      frame.identifier_format != vehicle_core::CanIdentifierFormat::Standard ||
      frame.identifier > 0x7ffU || bit_length == 0 || bit_length > 64 ||
      start_bit >= kPayloadBits ||
      (byte_order != ByteOrder::Intel && byte_order != ByteOrder::Motorola)) {
    return false;
  }

  const std::size_t payload_bits = static_cast<std::size_t>(frame.dlc) * 8U;
  std::size_t position = start_bit;
  std::uint64_t bits = 0;
  for (std::uint8_t index = 0; index < bit_length; ++index) {
    if (position >= payload_bits) {
      return false;
    }
    const std::uint8_t bit =
        static_cast<std::uint8_t>((frame.data[position / 8U] >> (position % 8U)) & 1U);
    if (byte_order == ByteOrder::Intel) {
      bits |= static_cast<std::uint64_t>(bit) << index;
      ++position;
    } else {
      bits = (bits << 1U) | bit;
      position = (position % 8U == 0U) ? position + 15U : position - 1U;
    }
  }

  output = RawValue{bits, bit_length, is_signed};
  return true;
}

bool append_result(DecodeResult *results, const std::size_t result_capacity,
                   std::size_t &result_count, const DecodeResult &result) noexcept {
  if (results == nullptr || result_count >= result_capacity) {
    return false;
  }
  results[result_count++] = result;
  return true;
}

bool signed_value(const RawValue &raw, std::int64_t &output) noexcept {
  SignedMagnitude value{};
  if (!magnitude_of(raw, value) || !raw.is_signed) {
    return false;
  }
  if (!value.negative) {
    output = static_cast<std::int64_t>(value.magnitude);
    return true;
  }

  constexpr std::uint64_t kInt64MinimumMagnitude = UINT64_C(1) << 63U;
  if (value.magnitude == kInt64MinimumMagnitude) {
    output = std::numeric_limits<std::int64_t>::min();
  } else {
    output = -static_cast<std::int64_t>(value.magnitude);
  }
  return true;
}

bool convert_number(const RawValue &raw, const NumberConversion &conversion,
                    const RawRange &permitted_range, const RawRange *invalid_ranges,
                    const std::size_t invalid_range_count, float &output) noexcept {
  bool range_is_valid = false;
  if (!valid_raw(raw) || !same_raw_shape(raw, conversion.pivot) || !finite(conversion.scale) ||
      !finite(conversion.target_at_pivot) ||
      (conversion.scale.high == 0.0L && conversion.scale.low == 0.0L) ||
      !in_range(raw, permitted_range, range_is_valid) || !range_is_valid ||
      !in_range(conversion.pivot, permitted_range, range_is_valid) || !range_is_valid ||
      (invalid_range_count != 0 && invalid_ranges == nullptr)) {
    return false;
  }

  for (std::size_t index = 0; index < invalid_range_count; ++index) {
    bool invalid_range_is_valid = false;
    const bool invalid = in_range(raw, invalid_ranges[index], invalid_range_is_valid);
    if (!invalid_range_is_valid || invalid) {
      return false;
    }
  }

  ExtendedValue delta{};
  if (!raw_delta(raw, conversion.pivot, delta)) {
    return false;
  }
  const ExtendedValue target = add(multiply(delta, conversion.scale), conversion.target_at_pivot);
  if (!finite(target)) {
    return false;
  }

  const ExtendedValue absolute_target =
      compare(target, ExtendedValue{0.0L}) < 0 ? negate(target) : target;
  if (compare(absolute_target,
              ExtendedValue{static_cast<long double>(std::numeric_limits<float>::max())}) > 0) {
    return false;
  }
  const long double target_value = target.high + target.low;
  const long double minimum_float =
      static_cast<long double>(std::numeric_limits<float>::denorm_min());
  if (!std::isfinite(target_value) || (compare(absolute_target, ExtendedValue{minimum_float}) < 0 &&
                                       compare(absolute_target, ExtendedValue{0.0L}) != 0)) {
    return false;
  }

  const float converted = static_cast<float>(target_value);
  if (!std::isfinite(converted) || (target_value != 0.0L && converted == 0.0F)) {
    return false;
  }
  output = converted;
  return true;
}

bool convert_boolean(const RawValue &raw, const NumberConversion &conversion,
                     const RawRange &permitted_range, const RawRange *invalid_ranges,
                     const std::size_t invalid_range_count, bool &output) noexcept {
  float number = 0.0F;
  if (!convert_number(raw, conversion, permitted_range, invalid_ranges, invalid_range_count,
                      number) ||
      (number != 0.0F && number != 1.0F)) {
    return false;
  }
  output = number == 1.0F;
  return true;
}

bool map_enum(const RawValue &raw, const RawRange &permitted_range, const RawRange *invalid_ranges,
              const std::size_t invalid_range_count, const EnumMapping *mappings,
              const std::size_t mapping_count, vehicle_signals::SignalValue &output) noexcept {
  bool range_is_valid = false;
  if (!valid_raw(raw) || (raw.bits & ~width_mask(raw.bit_length)) != 0 ||
      !in_range(raw, permitted_range, range_is_valid) || !range_is_valid ||
      (invalid_range_count != 0 && invalid_ranges == nullptr) || mappings == nullptr ||
      mapping_count == 0) {
    return false;
  }
  for (std::size_t index = 0; index < invalid_range_count; ++index) {
    bool invalid_range_is_valid = false;
    const bool invalid = in_range(raw, invalid_ranges[index], invalid_range_is_valid);
    if (!invalid_range_is_valid || invalid) {
      return false;
    }
  }

  const EnumMapping *match = nullptr;
  for (std::size_t index = 0; index < mapping_count; ++index) {
    if ((mappings[index].raw_bits & ~width_mask(raw.bit_length)) != 0) {
      return false;
    }
    if (mappings[index].raw_bits == raw.bits) {
      if (match != nullptr) {
        return false;
      }
      match = &mappings[index];
    }
  }
  if (match == nullptr) {
    return false;
  }
  output = vehicle_signals::SignalValue::enumeration(match->choice_value);
  return true;
}

} // namespace vehicle_profile_runtime
