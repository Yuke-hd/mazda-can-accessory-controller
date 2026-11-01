#include <array>
#include <cstddef>
#include <cstdint>

#include "vehicle_profile_runtime.hpp"

namespace {

#if !defined(PROFILE_EMPTY)
const vehicle_profile_runtime::DispatchEntry *find_entry(const std::uint32_t identifier) {
  for (const auto &entry : vehicle_profile::kDispatchTable) {
    if (entry.identifier == identifier) {
      return &entry;
    }
  }
  return nullptr;
}

bool decode(const std::uint32_t identifier, const std::array<std::uint8_t, 8> &data,
            const std::uint8_t dlc, std::array<vehicle_profile_runtime::DecodeResult, 3> &results,
            std::size_t &count) {
  const auto *const entry = find_entry(identifier);
  if (entry == nullptr || entry->expected_dlc != dlc || entry->decode == nullptr) {
    return false;
  }
  vehicle_core::RawCanFrame frame{};
  frame.identifier = identifier;
  frame.data = data;
  frame.dlc = dlc;
  entry->decode(frame, results.data(), results.size(), count);
  return true;
}
#endif

} // namespace

int main() {
#if defined(PROFILE_EMPTY)
  return vehicle_profile::kCatalog.empty() && vehicle_profile::kDispatchTable.empty() &&
                 vehicle_profile::kFreshnessPolicies.empty() &&
                 vehicle_profile::kMaxDecodeResults == 0
             ? 0
             : 1;
#else
  if (vehicle_profile::kCatalog.size() != 3 || vehicle_profile::kDispatchTable.size() != 3 ||
      vehicle_profile::kMaxDecodeResults != 1 || vehicle_profile::kBindingCount != 3 ||
      vehicle_profile::kFreshnessPolicies.size() != 3 ||
      vehicle_profile::kArtifactIdentity.size() != 64 ||
      vehicle_profile::kCompatibilityIdentity.size() != 64) {
    return 1;
  }

  bool saw_declared_timeout = false;
  bool saw_unverified = false;
  for (const auto &policy : vehicle_profile::kFreshnessPolicies) {
    if (policy.id.value() == 3 && policy.declared && policy.timeout_ms == 500) {
      saw_declared_timeout = true;
    }
    if (policy.id.value() == 1 && !policy.declared && policy.timeout_ms == 0) {
      saw_unverified = true;
    }
  }
  if (!saw_declared_timeout || !saw_unverified) {
    return 2;
  }

  std::array<vehicle_profile_runtime::DecodeResult, 3> results{};
  std::size_t count = 0;
  std::array<std::uint8_t, 8> data{};

  // These vectors are independent of generated pivot constants.
#if defined(PROFILE_SMALL_SCALE_WIDE)
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 3 || results[0].value.as_number() != 0.0F) {
    return 3;
  }
  data[0] = 1;
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 3 || results[0].value.as_number() != 0.1F) {
    return 4;
  }
  data = {};
  data[0] = 125;
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || results[0].valid ||
      results[0].id.value() != 3) {
    return 5;
  }
#elif defined(PROFILE_SIGNED_NEGATIVE_SCALE)
  // INT64_MIN under scale -0.25 maps exactly to +2^61.
  data[7] = 0x80;
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 3 || results[0].value.as_number() != 2305843009213693952.0F) {
    return 3;
  }
  // INT64_MAX maps to -2^61 after the required float32 rounding.
  data.fill(0xff);
  data[7] = 0x7f;
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 3 || results[0].value.as_number() != -2305843009213693952.0F) {
    return 4;
  }
  // The source-physical invalid interval [-0.5, 0] maps to signed raw [0, 2].
  data = {};
  data[0] = 1;
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || results[0].valid ||
      results[0].id.value() != 3) {
    return 5;
  }
#else
  // raw = 2^63 + 51; pivot-centered conversion preserves the exact offset cancellation.
  data[0] = 0x33;
  data[7] = 0x80;
#if defined(PROFILE_NEGATIVE_SCALE)
  constexpr float expected_number = -12.75F;
#else
  constexpr float expected_number = 12.75F;
#endif
  if (!decode(0x100, data, 8, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 3 || results[0].value.as_number() != expected_number) {
    return 3;
  }

  // raw = 2^63 + 50 is in the exact Fraction-derived invalid interval.
  data[0] = 0x32;
  count = 0;
  if (!decode(0x100, data, 8, results, count) || count != 1 || results[0].valid ||
      results[0].id.value() != 3) {
    return 4;
  }
#endif

  data = {};
  data[0] = 1;
  count = 0;
  if (!decode(0x101, data, 1, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 1 || results[0].value.as_boolean() != true) {
    return 5;
  }

  // Motorola start bit 7 walks MSB-first, so 0x40 encodes raw value 1.
  data = {};
  data[0] = 0x40;
  count = 0;
  if (!decode(0x102, data, 1, results, count) || count != 1 || !results[0].valid ||
      results[0].id.value() != 2 || results[0].value.as_enumeration() != 7) {
    return 6;
  }

  // Capacity exhaustion is handled through the R1 append_result contract.
  const auto *const number = find_entry(0x100);
  vehicle_core::RawCanFrame frame{};
  frame.identifier = 0x100;
  frame.data = {};
  frame.dlc = 8;
  count = 0;
  number->decode(frame, nullptr, 0, count);
  return count == 0 ? 0 : 7;
#endif
}
