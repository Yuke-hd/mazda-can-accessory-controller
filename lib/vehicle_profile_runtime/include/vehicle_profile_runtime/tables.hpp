#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "vehicle_core/frame.hpp"
#include "vehicle_signals/signal_contracts.hpp"

namespace vehicle_profile_runtime {

enum class ByteOrder : std::uint8_t { Intel, Motorola };

struct RawValue final {
  std::uint64_t bits{0};
  std::uint8_t bit_length{0};
  bool is_signed{false};
};

// Fixed two-part value used for generated affine constants. The host generator
// derives these parts from validated exact decimals; runtime arithmetic does
// not parse decimal text or allocate storage.
struct ExtendedValue final {
  long double high{0.0L};
  long double low{0.0L};

  constexpr ExtendedValue() noexcept = default;
  constexpr ExtendedValue(long double value) noexcept : high(value) {}
  constexpr ExtendedValue(long double high_part, long double low_part) noexcept
      : high(high_part), low(low_part) {}
};

// Inclusive range in the extracted raw integer domain. The profile generator
// derives allowed bounds from field minimum/maximum and invalid intervals from
// source-physical invalid ranges using exact host arithmetic. It must account
// for negative source scales when converting physical intervals to raw ranges.
struct RawRange final {
  RawValue minimum{};
  RawValue maximum{};
};

struct NumberConversion final {
  // The target affine transform is pivot-centered:
  // target = (raw - pivot) * scale + target_at_pivot.
  RawValue pivot{};
  ExtendedValue scale{1.0L};
  ExtendedValue target_at_pivot{};
};

struct EnumMapping final {
  // The raw value encoded in the signal width's unsigned bit representation.
  std::uint64_t raw_bits{0};
  std::uint16_t choice_value{0};
};

struct DecodeResult final {
  vehicle_signals::SignalId id{};
  vehicle_signals::SignalValue value{};
  bool valid{false};

  [[nodiscard]] static constexpr DecodeResult
  success(vehicle_signals::SignalId signal_id, vehicle_signals::SignalValue signal_value) noexcept {
    return DecodeResult{signal_id, signal_value, true};
  }

  [[nodiscard]] static constexpr DecodeResult
  invalid(vehicle_signals::SignalId signal_id) noexcept {
    return DecodeResult{signal_id, {}, false};
  }
};

// The caller initializes result_count to zero and owns the bounded result
// array. A decoder must stop when append_result returns false.
using DecodeFunction = void (*)(const vehicle_core::RawCanFrame &frame, DecodeResult *results,
                                std::size_t result_capacity, std::size_t &result_count);

struct DispatchEntry final {
  std::uint32_t identifier{0};
  std::uint8_t expected_dlc{0};
  DecodeFunction decode{nullptr};
  // Exact result ownership for this message. Generated tables keep this array
  // static so the provider can invalidate only the affected bindings when a
  // frame or decoder result is malformed.
  const vehicle_signals::SignalId *output_ids{nullptr};
  std::size_t output_count{0};
};

// One row per catalog signal. Imported profiles carry only the user's optional
// timeout claim; an undeclared timeout leaves freshness unverified. This type
// has no Mazda-specific exception or reviewed-timing implication.
struct SignalFreshnessPolicy final {
  vehicle_signals::SignalId id{};
  bool declared{false};
  std::uint32_t timeout_ms{0};
};

// Extracts one field from a standard classic-CAN data frame. On failure,
// output remains unchanged. Intel start bits identify the LSB; Motorola start
// bits identify the MSB and follow the DBC sawtooth walk.
[[nodiscard]] bool extract_raw_value(const vehicle_core::RawCanFrame &frame, std::uint8_t start_bit,
                                     std::uint8_t bit_length, ByteOrder byte_order, bool is_signed,
                                     RawValue &output) noexcept;

// Writes at most one result. It returns false without changing the array or
// count when capacity is exhausted or the output pointer is null.
[[nodiscard]] bool append_result(DecodeResult *results, std::size_t result_capacity,
                                 std::size_t &result_count, const DecodeResult &result) noexcept;

// Converts a signed raw field without signed overflow. Returns false when the
// raw record is not a valid signed field.
[[nodiscard]] bool signed_value(const RawValue &raw, std::int64_t &output) noexcept;

// Applies a pivot-centered target conversion after exact raw-domain range
// checks. Permitted bounds and invalid ranges are inclusive. The host generator
// emits exact raw intervals for source bounds/invalid ranges and fixed two-part
// affine constants for target = (raw - pivot) * scale + target_at_pivot. The
// pivot reduces cancellation; runtime never parses decimal text or allocates.
// Non-finite or unrepresentable float32 results fail and leave output unchanged.
[[nodiscard]] bool convert_number(const RawValue &raw, const NumberConversion &conversion,
                                  const RawRange &permitted_range, const RawRange *invalid_ranges,
                                  std::size_t invalid_range_count, float &output) noexcept;

[[nodiscard]] bool convert_boolean(const RawValue &raw, const NumberConversion &conversion,
                                   const RawRange &permitted_range, const RawRange *invalid_ranges,
                                   std::size_t invalid_range_count, bool &output) noexcept;

// Maps exact raw bit patterns after the same inclusive raw-domain checks used
// by Number and Boolean bindings. Unmapped or ambiguous values are invalid.
[[nodiscard]] bool map_enum(const RawValue &raw, const RawRange &permitted_range,
                            const RawRange *invalid_ranges, std::size_t invalid_range_count,
                            const EnumMapping *mappings, std::size_t mapping_count,
                            vehicle_signals::SignalValue &output) noexcept;

} // namespace vehicle_profile_runtime
