#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "action_engine/action.hpp"

namespace local_argb_actions {

// The strip regions the local renderer can light. These are LED concepts, so
// they live in this adapter and never in the engine or the signal layer.
enum class LedEffect : std::uint8_t { LeftTurn, RightTurn, Brake };

enum class BindingStatus : std::uint8_t {
  Ok,
  InvalidAction,    // ActionId zero.
  DuplicateBinding, // The same action already drives this effect.
  CapacityExceeded, // kCapacity bindings are already registered.
};

// Which effects are lit.
struct LedEffects {
  bool left_turn{false};
  bool right_turn{false};
  bool brake{false};

  [[nodiscard]] constexpr bool any() const noexcept { return left_turn || right_turn || brake; }
};

// Fixed-capacity set of (action, effect) bindings and the level last received
// for each action. An effect is lit while any action bound to it is active, so
// one action can drive several effects (hazard lights both turn regions) and
// several actions can drive one effect.
class EffectBindings final {
public:
  static constexpr std::size_t kCapacity = 8;

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action, LedEffect effect) noexcept;
  // Records the level of every binding of `action`. Returns false, and changes
  // nothing, when the action is not bound.
  [[nodiscard]] bool hold(action_engine::ActionId action, bool active) noexcept;
  [[nodiscard]] LedEffects lit() const noexcept;

private:
  struct Binding {
    action_engine::ActionId action{};
    LedEffect effect{LedEffect::LeftTurn};
    bool active{false};
  };

  [[nodiscard]] bool contains(action_engine::ActionId action, LedEffect effect) const noexcept;

  std::array<Binding, kCapacity> bindings_{};
  std::size_t count_{0};
};

} // namespace local_argb_actions
