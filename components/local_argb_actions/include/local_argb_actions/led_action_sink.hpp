#pragma once

#include <cstddef>

#include "action_engine/action.hpp"
#include "local_argb/lighting_sink.hpp"
#include "local_argb_actions/effect_bindings.hpp"
#include "local_argb_actions/fill_bindings.hpp"
#include "local_argb_actions/transient_bindings.hpp"

namespace local_argb_actions {

// Local LED output adapter: an action_engine::ActionSink that turns engine
// levels into the local renderer's private LightingCommand handoff. It knows
// no vehicle make, signal, CAN frame or rule; the composition root binds
// configured ActionIds to LED effects.
//
// Fail-off policy (held level): Activate lights the bound effects until the
// engine sends Deactivate, so every lit command is held with no deadline.
// Loss of data is handled upstream: the engine turns every NoData, Stale or
// Unavailable reading into Deactivate, which clears its held effects. A
// transient still completes within its own duration. Nothing here bounds a provider that stops
// delivering notices; the composition root has the renderer watch the provider's dispatcher
// progress (local_argb::watch_progress()), which fails off and rejects publishes while it is
// stalled. Stopping the provider or detaching the engine sends no Deactivate: the composition root
// must fail the renderer off (local_argb::fail_off()) when it does either. See
// docs/specs/lighting/local-led-actions.md.
//
// Every command for a bound action publishes the full effect state, so the
// engine's explicit initial Deactivate sets black before any transient starts. On/off effects
// follow Activate and Deactivate and ignore SetLevel. Fill effects take
// SetLevel as a fill level (see fill_level()); Activate fills the zone and
// Deactivate empties it. Trigger starts/restarts every transient bound to the
// action without changing its held effects or fills. Other commands leave
// transients unchanged; commands for unbound actions publish nothing.
// Latest transient starts remain in every full snapshot so an overwrite queue
// cannot lose them. Renderer timing expires each start independently and does
// not replay unchanged sequences after completion, cancellation or failure.
//
// Precondition: while the engine is attached, this adapter is the lighting
// sink's only publisher, so start the renderer first. The sink accepts every
// publish except while the renderer's progress watch has closed it. A
// rejected publish is not retried, and the engine does not resend a
// deduplicated level. The next full snapshot retains an unseen start, which
// begins at its later renderer application time. Already seen sequences
// cannot replay after renderer cancellation or failure.
//
// Setup: bind() runs before the engine attaches. execute() then runs on the
// engine's serialized command context, never blocks, and publishes through
// the borrowed lighting sink, which must outlive this adapter.
class LedActionSink final : public action_engine::ActionSink {
public:
  static constexpr std::size_t kMaxBindings = EffectBindings::kCapacity;
  static constexpr std::size_t kMaxFillBindings = FillBindings::kCapacity;
  static constexpr std::size_t kMaxTransientBindings = TransientBindings::kCapacity;

  explicit LedActionSink(local_argb::internal::LightingSink &lighting) noexcept
      : lighting_(&lighting) {}

  LedActionSink(const LedActionSink &) = delete;
  LedActionSink &operator=(const LedActionSink &) = delete;
  LedActionSink(LedActionSink &&) = delete;
  LedActionSink &operator=(LedActionSink &&) = delete;
  ~LedActionSink() = default;

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action, LedEffect effect,
                                   local_argb::internal::EffectPriority priority = {}) noexcept {
    return bindings_.bind(action, effect, priority);
  }
  [[nodiscard]] BindingStatus bind(action_engine::ActionId action,
                                   const FillEffect &effect) noexcept {
    return fills_.bind(action, effect);
  }

  [[nodiscard]] BindingStatus bind(action_engine::ActionId action,
                                   const local_argb::internal::LightingTransient &effect) noexcept {
    return transients_.bind(action, effect);
  }

  void execute(const action_engine::ActionCommand &command) noexcept override;

private:
  local_argb::internal::LightingSink *lighting_;
  EffectBindings bindings_{};
  FillBindings fills_{};
  TransientBindings transients_{};
};

} // namespace local_argb_actions
