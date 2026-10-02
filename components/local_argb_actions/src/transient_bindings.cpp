#include "local_argb_actions/transient_bindings.hpp"

#include "local_argb/pixel_frame.hpp"

namespace local_argb_actions {

BindingStatus
TransientBindings::bind(const action_engine::ActionId action,
                        const local_argb::internal::LightingTransient &effect) noexcept {
  if (!action.valid())
    return BindingStatus::InvalidAction;
  const auto &zone = effect.zone;
  if (effect.duration_us == 0 ||
      effect.duration_us > local_argb::internal::kMaxTransientDurationUs || zone.length == 0 ||
      zone.start >= local_argb::kLedCount || zone.length > local_argb::kLedCount - zone.start ||
      (zone.direction != local_argb::internal::FillDirection::StartToEnd &&
       zone.direction != local_argb::internal::FillDirection::EndToStart &&
       zone.direction != local_argb::internal::FillDirection::CenterOut))
    return BindingStatus::InvalidEffect;
  if (contains(action, effect.zone))
    return BindingStatus::DuplicateBinding;
  if (count_ == kCapacity)
    return BindingStatus::CapacityExceeded;
  bindings_[count_++] = Binding{action, effect, 0};
  return BindingStatus::Ok;
}

bool TransientBindings::trigger(const action_engine::ActionId action,
                                const vehicle_core::MonotonicTimestamp origin_us,
                                const std::uint32_t epoch) noexcept {
  bool bound = false;
  for (std::size_t index = 0; index < count_; ++index) {
    Binding &binding = bindings_[index];
    if (binding.action != action)
      continue;
    ++binding.sequence;
    if (binding.sequence == 0)
      ++binding.sequence;
    binding.origin_us = origin_us;
    binding.epoch = epoch;
    binding.retained = true;
    bound = true;
  }
  return bound;
}

local_argb::internal::LightingTransientStarts
TransientBindings::latest(const std::uint32_t epoch) const noexcept {
  local_argb::internal::LightingTransientStarts starts{};
  for (std::size_t index = 0; index < count_; ++index) {
    const Binding &binding = bindings_[index];
    if (!binding.retained || binding.epoch != epoch)
      continue;
    // Unique slot IDs and matching capacities make this append infallible.
    (void)starts.add({local_argb::internal::TransientId{static_cast<std::uint8_t>(index + 1)},
                      binding.sequence, binding.effect, binding.origin_us, binding.epoch});
  }
  return starts;
}

void TransientBindings::discard_starts() noexcept {
  for (std::size_t index = 0; index < count_; ++index)
    bindings_[index].retained = false;
}

bool TransientBindings::contains(const action_engine::ActionId action,
                                 const local_argb::internal::LedZone &zone) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (bindings_[index].action == action && bindings_[index].effect.zone.start == zone.start &&
        bindings_[index].effect.zone.length == zone.length)
      return true;
  }
  return false;
}

} // namespace local_argb_actions
