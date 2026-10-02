#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "action_engine/action.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/effect_bindings.hpp"

namespace local_argb_actions {

// Setup-time bindings and latest starts. Keeping each start in every snapshot
// prevents a subsequent held update from overwriting it in the renderer queue.
class TransientBindings final {
public:
  static constexpr std::size_t kCapacity = local_argb::internal::LightingTransientStarts::kCapacity;

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action,
                                   const local_argb::internal::LightingTransient &effect) noexcept;
  [[nodiscard]] bool trigger(action_engine::ActionId action) noexcept;
  [[nodiscard]] local_argb::internal::LightingTransientStarts latest() const noexcept;

private:
  struct Binding {
    action_engine::ActionId action{};
    local_argb::internal::LightingTransient effect{};
    std::uint64_t sequence{0};
  };

  [[nodiscard]] bool contains(action_engine::ActionId action,
                              const local_argb::internal::LedZone &zone) const noexcept;

  std::array<Binding, kCapacity> bindings_{};
  std::size_t count_{0};
};

} // namespace local_argb_actions
