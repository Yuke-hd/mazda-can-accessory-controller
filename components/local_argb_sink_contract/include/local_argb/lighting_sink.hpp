#pragma once

#include <cstdint>

#include "vehicle_core/time.hpp"

namespace local_argb::internal {

struct LightingRgb {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

struct LightingCommand {
  // color is retained for the generic solid-colour compatibility handoff.
  LightingRgb color{};
  // These effect flags are the vehicle-strip handoff used by the firmware.
  bool left_turn{false};
  bool right_turn{false};
  bool brake{false};
  vehicle_core::MonotonicTimestamp valid_until_us{0};
  bool actionable{false};
};

// Private, value-only sink between the portable lighting policy and the
// renderer. Mazda enums, decoder health, and driver handles do not cross it.
class LightingSink {
public:
  virtual ~LightingSink() = default;
  virtual bool publish(const LightingCommand &command) noexcept = 0;
};

// Adapt an implementation-owned generic solid-colour command without exposing
// either module's private class to the other. Vehicle-specific effect flags
// are populated by the explicit firmware binding instead.
template <typename GenericCommand>
[[nodiscard]] inline LightingCommand adapt_command(const GenericCommand &source) noexcept {
  return LightingCommand{{source.color.red, source.color.green, source.color.blue},
                         false,
                         false,
                         false,
                         source.valid_until_us,
                         source.actionable};
}

// The service obtains this implementation-only handoff explicitly. Ordinary
// facade consumers only see the generic public renderer values, not the sink
// operation or queue ownership.
[[nodiscard]] LightingSink &sink() noexcept;

} // namespace local_argb::internal
