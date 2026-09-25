# Local LED action sink

`components/local_argb_actions` (#11) is the output adapter between the
generic action engine and the local ARGB renderer. It implements
`action_engine::ActionSink` and publishes the renderer's private
`local_argb::internal::LightingCommand` through a borrowed
`local_argb::internal::LightingSink`.

```text
composition root   binds ActionIds to LED effects and wires the parts
  |
  v
ActionEngine --ActionCommand--> LedActionSink --LightingCommand--> LightingSink
                                                                     |
                                   RendererController --PixelFrame--> PixelFrameSink
```

The adapter knows no vehicle make, signal, rule or CAN frame. The engine and
the signal layer know no LED effect. Only the composition root knows both.

## Bindings

`LedActionSink::bind(ActionId, LedEffect)` maps a configured action to one of
the renderer's effects: `LeftTurn`, `RightTurn` or `Brake`. An effect is lit
while any action bound to it is active, and one action may drive several
effects. For example, a hazard action bound to both turn effects lights both
regions. `bind()` returns:

| Status | Cause |
| --- | --- |
| `InvalidAction` | The `ActionId` is zero. |
| `DuplicateBinding` | The same action already drives this effect. |
| `CapacityExceeded` | `LedActionSink::kMaxBindings` (8) bindings exist. |

Bind every action before the engine attaches. `LedEffect` names strip
regions, not vehicle sides: the WeAct strip is mounted mirrored, so the
firmware binds the vehicle's left turn to `RightTurn`, as the legacy binding
did.

## Commands

- `Activate` and `Deactivate` set the level of every binding of the action.
  Each one publishes the full effect state, not a delta.
- `Trigger` and `SetLevel` are ignored. LED effects are on/off levels, and
  the renderer has no progress effect yet.
- Commands for unbound actions are ignored and publish nothing.

The engine's explicit initial `Deactivate` therefore publishes a black
baseline on each provider start.

While the engine is attached, the adapter must be the lighting sink's only
publisher, and the sink must accept every publish. The production sink is a
one-slot overwrite queue that rejects only before `local_argb::start()`, so
start the renderer before the engine attaches. A rejected publish is not
retried, and the engine never resends a deduplicated level, so a rejected
`Deactivate` would leave the effect lit.

## Fail-off policy: held level

Engine levels carry no deadline and are deduplicated, so the adapter publishes
every lit command as *held*: `valid_until_us` is the maximum timestamp and the
renderer keeps animating until a later command turns the effect off. An
all-off command is non-actionable and renders black.

Loss of data fails off through the engine, not through a renderer deadline:

- The engine turns every NoData, Stale or Unavailable reading into
  `Deactivate` (see [action-engine.md](action-engine.md#availability-policy)).
  With the Mazda provider, the turn state becomes Stale after its 250 ms
  freshness timeout. Transport silence, a faulted transport and a decoder
  fault on the turn message make it Unavailable. The running provider
  publishes these transitions without a new frame.
- A `FreshnessUnverified` reading lights an effect only if the rule opts in
  with `FreshnessRequirement::FreshOrUnverified`.
- Renderer startup black and write-failure fail-off are unchanged.

The composition root owns the cases the engine cannot see:

- Stopping the provider publishes no Unavailable notice, and
  `ActionEngine::detach()` sends no `Deactivate`. Call `local_argb::fail_off()`
  after the provider's `stop()` or the engine's `detach()` returns, so that no
  late command can relight the strip.
- If notice delivery stops while the provider runs (a hung dispatcher), a
  held effect stays lit until a reset or a later command. The renderer's
  watchdogs cover only a stuck LED driver or LED worker, not the provider.
  The legacy binding published from the receive worker with a per-command
  deadline of at most the 250 ms turn freshness or the transport-silence
  timeout. Restoring such a bound needs a provider liveness signal, which is
  outside #11.

Write-fault recovery also differs from the legacy binding. After a failed
pixel write the renderer writes black and latches its fault until the next
command. The legacy heartbeat re-applied its command every 100 ms and so
relit the strip quickly. With held levels, the strip stays dark until the next
level change. This is fail-safe but visible.

## Evidence

- `tests/host/local_led_action_sink_tests.cpp` covers bindings, OR-ed effects,
  the explicit black baseline, ignored Trigger/SetLevel/unbound commands, a
  rejected publish, and the binding errors.
- `tests/host/local_led_action_composition_tests.cpp` runs a make-independent
  fake provider, the engine, the adapter and the production
  `RendererController` into a fake `PixelFrameSink`. It shows left, right and
  hazard turn states rendered as pixels, a held effect with no deadline, and
  black for off, NoData, Stale, Unavailable and rejected unverified readings,
  plus recovery and the latched write fault.
- `architecture_contracts` restricts the adapter to the engine action port,
  the renderer sink contract, core time values and standard headers. It
  rejects any provider, Mazda, CAN, LED driver, RTOS/SDK or WLED dependency,
  and any other link target, include directory or ESP-IDF requirement.

## Firmware composition

`firmware/weact-can485-v1.1/main/main.cpp` is the only code that knows the
Mazda provider, the engine and this adapter. In static storage it builds
`mazda::MazdaSignalProvider` over the telemetry facade, an
`action_engine::ActionEngine` over the provider, and a `LedActionSink` over
the renderer queue `local_argb::internal::sink()`. Before CAN starts it:

1. binds the mirrored effects: `left` to `RightTurn`, `right` to `LeftTurn`,
   and `hazard` to both;
2. adds the sink and three state rules, `vehicle.turn_state Equal left`,
   `right` and `hazard`, with the strict `Fresh` requirement;
3. registers the typed turn notice log and attaches the engine. These are the
   turn channel's two subscriber slots;
4. starts the facade.

Any setup failure calls `local_argb::fail_off()` and refuses to start CAN.
The application never stops the facade or detaches the engine.

The migration keeps the visible turn and hazard behavior of the legacy
`mazda::application::bind_local_argb_sink()` binding. That binding also lit
brake on a Fresh brake reading. Brake has no freshness timeout, so it is never
Fresh and never lit. Brake is also not in the generic catalog, so the
migration drops it with no visible change.

The legacy binding is no longer bound in firmware. The renderer queue has one
slot, so the LED sink must be its only publisher. The binding stays compiled
in `mazda_telemetry` as a rollout fallback and is to be removed after rollout.
The local ARGB boundary validator rejects it in the vehicle application. It
also requires the wiring above: the components, attach before start, fail-off
on every setup failure and after any stop or detach, the mirrored bindings,
no `FreshOrUnverified`, and no subscription inside the adapter.
`tests/tools/validate_local_argb_boundary_test.py` covers these rules.
