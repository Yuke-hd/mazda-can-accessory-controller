#pragma once

#include <cstdint>

#include "vehicle_core/time.hpp"

namespace vehicle_lighting_policy {

// Generic value-only output from the Mazda policy.  The renderer-facing sink
// owns its private handoff record; keeping this record in the policy module
// prevents vehicle_core from depending on an application effect.
struct LightingRgb {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

constexpr bool operator==(const LightingRgb &left, const LightingRgb &right) noexcept {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

constexpr bool operator!=(const LightingRgb &left, const LightingRgb &right) noexcept {
  return !(left == right);
}

// valid_until_us is an absolute monotonic deadline.  An actionable command
// is expired once now_us is greater than this value.  Non-actionable commands
// are rendered black regardless of their deadline.
struct LightingCommand {
  LightingRgb color{};
  vehicle_core::MonotonicTimestamp valid_until_us{0};
  bool actionable{false};
};

constexpr bool operator==(const LightingCommand &left, const LightingCommand &right) noexcept {
  return left.color == right.color && left.valid_until_us == right.valid_until_us &&
         left.actionable == right.actionable;
}

constexpr bool operator!=(const LightingCommand &left, const LightingCommand &right) noexcept {
  return !(left == right);
}

} // namespace vehicle_lighting_policy
