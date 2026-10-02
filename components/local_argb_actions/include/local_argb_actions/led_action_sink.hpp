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
// Latest starts retain their Trigger clock and sink cancellation epoch in later
// snapshots. The renderer admits an unseen start only inside its original
// duration window and current epoch; it never crosses a stall or lifecycle
// fail-off. Completed starts cannot replay. A rejected publication retires all
// pending starts; recovery requires a newer Trigger.
//
// Precondition: while the engine is attached, this adapter is the lighting
// sink's only publisher, so start the renderer first. The borrowed clock must
// use the renderer's monotonic microsecond timebase. The sink accepts every
// publish except while its progress gate is closed. A rejected held level is
// not retried, and the engine does not resend a deduplicated level.
//
// Setup: bind() runs before the engine attaches. execute() then runs on the
// engine's serialized command context, never blocks, and publishes through
// the borrowed lighting sink, which must outlive this adapter.
class LedActionSink final : public action_engine::ActionSink {
public:
  static constexpr std::size_t kMaxBindings = EffectBindings::kCapacity;
  static constexpr std::size_t kMaxFillBindings = FillBindings::kCapacity;
  static constexpr std::size_t kMaxTransientBindings = TransientBindings::kCapacity;

  LedActionSink(local_argb::internal::LightingSink &lighting,
                vehicle_core::MonotonicClock &clock) noexcept
      : lighting_(&lighting), clock_(&clock) {}

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
  vehicle_core::MonotonicClock *clock_;
  EffectBindings bindings_{};
  FillBindings fills_{};
  TransientBindings transients_{};
};

} // namespace local_argb_actions
