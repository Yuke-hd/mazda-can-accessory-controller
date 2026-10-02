#include "local_argb_actions/transient_bindings.hpp"

namespace local_argb_actions {

BindingStatus
TransientBindings::bind(const action_engine::ActionId action,
                        const local_argb::internal::LightingTransient &effect) noexcept {
  if (!action.valid())
    return BindingStatus::InvalidAction;
  if (contains(action, effect.zone))
    return BindingStatus::DuplicateBinding;
  if (count_ == kCapacity)
    return BindingStatus::CapacityExceeded;
  bindings_[count_++] = Binding{action, effect, 0};
  return BindingStatus::Ok;
}

bool TransientBindings::trigger(const action_engine::ActionId action) noexcept {
  bool bound = false;
  for (std::size_t index = 0; index < count_; ++index) {
    Binding &binding = bindings_[index];
    if (binding.action != action)
      continue;
    ++binding.sequence;
    if (binding.sequence == 0)
      ++binding.sequence;
    bound = true;
  }
  return bound;
}

local_argb::internal::LightingTransientStarts TransientBindings::latest() const noexcept {
  local_argb::internal::LightingTransientStarts starts{};
  for (std::size_t index = 0; index < count_; ++index) {
    const Binding &binding = bindings_[index];
    if (binding.sequence == 0)
      continue;
    // Unique slot IDs and matching capacities make this append infallible.
    (void)starts.add({local_argb::internal::TransientId{static_cast<std::uint8_t>(index + 1)},
                      binding.sequence, binding.effect});
  }
  return starts;
}

bool TransientBindings::contains(const action_engine::ActionId action,
                                 const local_argb::internal::LedZone &zone) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (bindings_[index].action == action && bindings_[index].effect.zone == zone)
      return true;
  }
  return false;
}

} // namespace local_argb_actions
