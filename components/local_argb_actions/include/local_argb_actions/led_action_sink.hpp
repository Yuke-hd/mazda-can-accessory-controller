#pragma once

#include <cstddef>

#include "action_engine/action.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/effect_bindings.hpp"

namespace local_argb_actions {

// Local LED output adapter: an action_engine::ActionSink that turns engine
// levels into the local renderer's private LightingCommand handoff. It knows
// no vehicle make, signal, CAN frame or rule; the composition root binds
// configured ActionIds to LED effects.
//
// Fail-off policy (held level): Activate lights the bound effects until the
// engine sends Deactivate, so every published command is held with no
// deadline. Loss of data is handled upstream: the engine turns every NoData,
// Stale or Unavailable reading into Deactivate, which publishes a
// non-actionable black command. Renderer startup black and write-failure
// fail-off are unchanged. A dispatcher that stops delivering notices leaves
// the last published frame in place; see docs/development/local-led-actions.md.
//
// Every command for a bound action publishes the full effect state, so the
// engine's explicit initial Deactivate sets a black baseline and a publish
// the lighting sink rejects is repaired by the next command. LED effects are
// on/off levels, so Trigger and SetLevel are ignored, as are commands for
// unbound actions.
//
// Setup: bind() runs before the engine attaches. execute() then runs on the
// engine's serialized command context, never blocks, and publishes through
// the borrowed lighting sink, which must outlive this adapter.
class LedActionSink final : public action_engine::ActionSink {
public:
  static constexpr std::size_t kMaxBindings = EffectBindings::kCapacity;

  explicit LedActionSink(local_argb::internal::LightingSink &lighting) noexcept
      : lighting_(&lighting) {}

  LedActionSink(const LedActionSink &) = delete;
  LedActionSink &operator=(const LedActionSink &) = delete;
  LedActionSink(LedActionSink &&) = delete;
  LedActionSink &operator=(LedActionSink &&) = delete;
  ~LedActionSink() = default;

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action, LedEffect effect) noexcept {
    return bindings_.bind(action, effect);
  }

  void execute(const action_engine::ActionCommand &command) noexcept override;

private:
  local_argb::internal::LightingSink *lighting_;
  EffectBindings bindings_{};
};

} // namespace local_argb_actions
