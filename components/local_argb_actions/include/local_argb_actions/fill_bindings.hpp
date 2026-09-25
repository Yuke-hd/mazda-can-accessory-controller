#pragma once

#include <array>
#include <cstddef>

#include "action_engine/action.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/effect_bindings.hpp"

namespace local_argb_actions {

// A level-capable LED effect: a configured strip zone, lit from its fill
// direction in proportion to the level, in one colour. The renderer caps the
// colour at its brightness ceiling and draws nothing for an invalid zone.
struct FillEffect {
  local_argb::internal::LedZone zone{};
  local_argb::internal::LightingRgb color{};
};

// Converts an engine level to a fill fraction. Levels are normalized to
// 0.0..1.0: values at or below zero, and NaN, are empty (fail-off); values at
// or above one, including +infinity, are full. Levels in between are rounded
// to the nearest 1/65536.
[[nodiscard]] local_argb::internal::FillFraction fill_level(float level) noexcept;

// Fixed-capacity set of (action, fill) bindings and the level last received
// for each. One action may drive several zones, and several actions may drive
// one zone; each binding is drawn separately, in binding order.
class FillBindings final {
public:
  static constexpr std::size_t kCapacity = local_argb::internal::LightingFills::kCapacity;

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action,
                                   const FillEffect &effect) noexcept;
  // Records `level` for every fill binding of `action`. Returns false, and
  // changes nothing, when the action drives no fill.
  [[nodiscard]] bool hold(action_engine::ActionId action,
                          local_argb::internal::FillFraction level) noexcept;
  // The non-empty fills, in binding order.
  [[nodiscard]] local_argb::internal::LightingFills lit() const noexcept;

private:
  struct Binding {
    action_engine::ActionId action{};
    FillEffect effect{};
    local_argb::internal::FillFraction level{local_argb::internal::FillFraction::empty()};
  };

  [[nodiscard]] bool contains(action_engine::ActionId action,
                              const local_argb::internal::LedZone &zone) const noexcept;

  std::array<Binding, kCapacity> bindings_{};
  std::size_t count_{0};
};

} // namespace local_argb_actions
