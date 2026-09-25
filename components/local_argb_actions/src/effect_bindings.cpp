#include "local_argb_actions/effect_bindings.hpp"

namespace local_argb_actions {

BindingStatus EffectBindings::bind(const action_engine::ActionId action,
                                   const LedEffect effect) noexcept {
  if (!action.valid())
    return BindingStatus::InvalidAction;
  if (contains(action, effect))
    return BindingStatus::DuplicateBinding;
  if (count_ == kCapacity)
    return BindingStatus::CapacityExceeded;
  bindings_[count_++] = Binding{action, effect, false};
  return BindingStatus::Ok;
}

bool EffectBindings::hold(const action_engine::ActionId action, const bool active) noexcept {
  bool bound = false;
  for (std::size_t index = 0; index < count_; ++index) {
    Binding &binding = bindings_[index];
    if (binding.action == action) {
      binding.active = active;
      bound = true;
    }
  }
  return bound;
}

LedEffects EffectBindings::lit() const noexcept {
  LedEffects effects{};
  for (std::size_t index = 0; index < count_; ++index) {
    const Binding &binding = bindings_[index];
    if (!binding.active)
      continue;
    effects.left_turn = effects.left_turn || binding.effect == LedEffect::LeftTurn;
    effects.right_turn = effects.right_turn || binding.effect == LedEffect::RightTurn;
    effects.brake = effects.brake || binding.effect == LedEffect::Brake;
  }
  return effects;
}

bool EffectBindings::contains(const action_engine::ActionId action,
                              const LedEffect effect) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (bindings_[index].action == action && bindings_[index].effect == effect)
      return true;
  }
  return false;
}

} // namespace local_argb_actions
