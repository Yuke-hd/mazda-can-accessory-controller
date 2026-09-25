#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "local_argb/lighting_zone.hpp"
#include "vehicle_core/time.hpp"

namespace local_argb::internal {

struct LightingRgb {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

// Resolves effects that overlap on the strip. Each lit effect owns the pixels
// of its region, dark pixels included, wherever no effect of higher priority
// overlaps it. At equal priority the later effect in drawing order wins: fills
// in list order, then brake, then the left turn, then the right turn.
// Priority is a binding choice; it does not depend on the signal behind it.
class EffectPriority {
public:
  static constexpr std::uint8_t kDefault = 100;

  constexpr EffectPriority() noexcept = default;
  constexpr explicit EffectPriority(const std::uint8_t rank) noexcept : rank_(rank) {}

  [[nodiscard]] constexpr std::uint8_t rank() const noexcept { return rank_; }

private:
  std::uint8_t rank_{kDefault};
};

constexpr bool operator==(const EffectPriority left, const EffectPriority right) noexcept {
  return left.rank() == right.rank();
}
constexpr bool operator!=(const EffectPriority left, const EffectPriority right) noexcept {
  return !(left == right);
}
constexpr bool operator<(const EffectPriority left, const EffectPriority right) noexcept {
  return left.rank() < right.rank();
}

// The priority of each fixed effect, used while that effect is lit.
struct EffectPriorities {
  EffectPriority left_turn{};
  EffectPriority right_turn{};
  EffectPriority brake{};
};

// One level-driven fill: `level` of `zone` lit in `color`. The renderer caps
// each colour channel at its brightness ceiling and draws nothing for an
// invalid zone.
struct LightingFill {
  LedZone zone{};
  FillFraction level{FillFraction::empty()};
  LightingRgb color{};
  EffectPriority priority{};
};

// Fixed-capacity, trivially copyable list of fills, in drawing order. It never
// allocates, so a LightingCommand still crosses the renderer queue by copy.
class LightingFills {
public:
  static constexpr std::size_t kCapacity = 8;

  // Appends `fill`; returns false, and changes nothing, when full.
  [[nodiscard]] bool add(const LightingFill &fill) noexcept {
    if (count_ == kCapacity)
      return false;
    fills_[count_++] = fill;
    return true;
  }

  [[nodiscard]] const LightingFill *begin() const noexcept { return fills_.data(); }
  [[nodiscard]] const LightingFill *end() const noexcept { return fills_.data() + count_; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

private:
  std::array<LightingFill, kCapacity> fills_{};
  std::size_t count_{0};
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
  // Level-driven zone fills. With no fills, no brake and no turn, the
  // renderer paints the generic colour instead.
  LightingFills fills{};
  // Overlap resolution for the fixed effects; see EffectPriority.
  EffectPriorities priorities{};
};

// The renderer queue copies commands byte-wise.
static_assert(std::is_trivially_copyable_v<LightingCommand>);

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
  LightingCommand command{};
  command.color = {source.color.red, source.color.green, source.color.blue};
  command.valid_until_us = source.valid_until_us;
  command.actionable = source.actionable;
  return command;
}

// The service obtains this implementation-only handoff explicitly. Ordinary
// facade consumers only see the generic public renderer values, not the sink
// operation or queue ownership.
[[nodiscard]] LightingSink &sink() noexcept;

} // namespace local_argb::internal
