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

Bind every action before the engine attaches.

## Commands

- `Activate` and `Deactivate` set the level of every binding of the action.
  Each one publishes the full effect state, not a delta.
- `Trigger` and `SetLevel` are ignored. LED effects are on/off levels, and
  the renderer has no progress effect yet.
- Commands for unbound actions are ignored and publish nothing.

The engine's explicit initial `Deactivate` therefore publishes a black
baseline on each provider start. If the lighting sink rejects a publish, the
adapter does not retry: the next command republishes the full state.

## Fail-off policy: held level

Engine levels carry no deadline and are deduplicated, so the adapter publishes
every lit command as *held*: `valid_until_us` is the maximum timestamp and the
renderer keeps animating until a later command turns the effect off. An
all-off command is non-actionable and renders black.

Loss of data fails off through the engine, not through a renderer deadline:

- The engine turns every NoData, Stale or Unavailable reading into
  `Deactivate` (see [action-engine.md](action-engine.md#availability-policy)).
  With the Mazda provider, the turn state becomes Stale after its 250 ms
  freshness timeout. Transport silence, a stopped or faulted transport, and a
  decoder fault make readings Unavailable. The provider publishes these
  transitions without a new frame.
- A `FreshnessUnverified` reading lights an effect only if the rule opts in
  with `FreshnessRequirement::FreshOrUnverified`.
- Renderer startup black, write-failure fail-off and its driver and worker
  watchdogs are unchanged.

Residual risk: the legacy binding also receives a 100 ms heartbeat, from the
telemetry service that publishes its deadlines. If notice delivery stops (a
hung dispatcher), a held effect stays lit until the renderer's watchdogs, a
reset or a later command intervene. This adapter does not detect that case.
Closing it needs a provider liveness signal, which is outside #11.

## Evidence

- `tests/host/local_led_action_sink_tests.cpp` covers bindings, OR-ed effects,
  the explicit black baseline, ignored Trigger/SetLevel/unbound commands, a
  rejected publish, and the binding errors.
- `tests/host/local_led_action_composition_tests.cpp` runs a make-independent
  fake provider, the engine, the adapter and the production
  `RendererController` into a fake `PixelFrameSink`. It shows left, right and
  hazard turn states rendered as pixels, a held effect with no deadline, and
  black for off, NoData, Stale, Unavailable and rejected unverified readings,
  plus recovery.
- `architecture_contracts` restricts the adapter to the engine action port,
  the renderer sink contract and standard headers. It rejects any provider,
  Mazda, CAN, RTOS or WLED dependency and any other link target.

Firmware composition-root wiring and migration of the current lighting
binding are a separate step of #11.
