#include "local_argb_actions/fill_bindings.hpp"

#include <cmath>
#include <cstdint>

namespace local_argb_actions {
namespace {

using local_argb::internal::FillFraction;

// Resolution of a fractional level: fine enough for any strip zone.
constexpr std::uint32_t kLevelSteps = 1U << 16U;

} // namespace

FillFraction fill_level(const float level) noexcept {
  // The negated comparison also sends NaN to empty.
  if (!(level > 0.0F))
    return FillFraction::empty();
  if (level >= 1.0F)
    return FillFraction::full();
  const auto steps = static_cast<std::uint32_t>(std::lround(level * kLevelSteps));
  return FillFraction::of(steps, kLevelSteps);
}

BindingStatus FillBindings::bind(const action_engine::ActionId action,
                                 const FillEffect &effect) noexcept {
  if (!action.valid())
    return BindingStatus::InvalidAction;
  if (contains(action, effect.zone))
    return BindingStatus::DuplicateBinding;
  if (count_ == kCapacity)
    return BindingStatus::CapacityExceeded;
  bindings_[count_++] = Binding{action, effect, FillFraction::empty()};
  return BindingStatus::Ok;
}

bool FillBindings::hold(const action_engine::ActionId action, const FillFraction level) noexcept {
  bool bound = false;
  for (std::size_t index = 0; index < count_; ++index) {
    Binding &binding = bindings_[index];
    if (binding.action == action) {
      binding.level = level;
      bound = true;
    }
  }
  return bound;
}

local_argb::internal::LightingFills FillBindings::lit() const noexcept {
  local_argb::internal::LightingFills fills{};
  for (std::size_t index = 0; index < count_; ++index) {
    const Binding &binding = bindings_[index];
    if (binding.level == FillFraction::empty())
      continue;
    // Cannot fail: both lists share one capacity.
    (void)fills.add({binding.effect.zone, binding.level, binding.effect.color});
  }
  return fills;
}

bool FillBindings::contains(const action_engine::ActionId action,
                            const local_argb::internal::LedZone &zone) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (bindings_[index].action == action && bindings_[index].effect.zone == zone)
      return true;
  }
  return false;
}

} // namespace local_argb_actions
