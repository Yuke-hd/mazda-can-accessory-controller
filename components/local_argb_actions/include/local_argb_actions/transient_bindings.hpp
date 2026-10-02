#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "action_engine/action.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/effect_bindings.hpp"

namespace local_argb_actions {

// Setup-time bindings and latest starts. Keeping each start in every snapshot
// prevents a held update from overwriting it in the renderer queue, within
// the original admission window and cancellation epoch. Rejection retires it.
class TransientBindings final {
public:
  static constexpr std::size_t kCapacity = local_argb::internal::LightingTransientStarts::kCapacity;

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action,
                                   const local_argb::internal::LightingTransient &effect) noexcept;
  [[nodiscard]] bool trigger(action_engine::ActionId action,
                             vehicle_core::MonotonicTimestamp origin_us,
                             std::uint32_t epoch) noexcept;
  [[nodiscard]] local_argb::internal::LightingTransientStarts
  latest(std::uint32_t epoch) const noexcept;
  void discard_starts() noexcept;

private:
  struct Binding {
    action_engine::ActionId action{};
    local_argb::internal::LightingTransient effect{};
    std::uint64_t sequence{0};
    vehicle_core::MonotonicTimestamp origin_us{0};
    std::uint32_t epoch{0};
    // Retained for snapshots until retired; renderer application does not clear it.
    bool retained{false};
  };

  [[nodiscard]] bool contains(action_engine::ActionId action,
                              const local_argb::internal::LedZone &zone) const noexcept;

  std::array<Binding, kCapacity> bindings_{};
  std::size_t count_{0};
};

} // namespace local_argb_actions
