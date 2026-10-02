# Local LED action sink

This document specifies the current output adapter in
`components/local_argb_actions`. It implements `action_engine::ActionSink` and
publishes the renderer's private `local_argb::internal::LightingCommand`
through a borrowed `local_argb::internal::LightingSink`. Construction also
borrows a `vehicle_core::MonotonicClock` using the renderer's monotonic
microsecond timebase; firmware adapts `esp_timer_get_time`, and replay shares
its existing replay clock.

```text
composition root   binds ActionIds to LED effects and wires the parts
  |
  v
ActionEngine --ActionCommand--> LedActionSink --LightingCommand--> LightingSink
                                                                     |
                                   RendererController --PixelFrame--> PixelFrameSink
```

The adapter knows no vehicle make, signal, rule or CAN frame. The engine and
the signal layer know no LED effect. Only the composition root knows both;
firmware construction and lifecycle are specified in
[firmware composition](../../architecture/firmware-composition.md).

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

### Transient effects

`LedActionSink::bind(ActionId, LightingTransient)` binds an action to a solid
zone with a colour, priority and duration in monotonic microseconds. Each
`Trigger` starts every transient bound to that action. A repeated `Trigger`
restarts them at renderer application time; it never queues another run.
Other command kinds leave these bindings unchanged. One action may drive
multiple transient zones alongside held or fill bindings.

The fixed capacity is `LedActionSink::kMaxTransientBindings` (8).
`InvalidAction` means action zero; `DuplicateBinding` means the same action
already drives the same physical zone (start and length), regardless of
fill direction, colour or duration. Excess bindings return `CapacityExceeded`.
`InvalidEffect` rejects an empty, out-of-range or unknown-direction zone,
zero duration, or duration above the shared five-second maximum
(`kMaxTransientDurationUs`). Invalid registration does not consume capacity.

Each binding owns a stable renderer slot and a nonzero sequence changed only
on Trigger. The adapter stamps the Trigger's monotonic origin and the sink's
cancellation epoch once. Later snapshots preserve those values so a held
update cannot erase a pending start through the one-slot overwrite queue.
The renderer admits an unseen start only while its origin is no older than
its duration (strictly less than the duration) and its epoch is current.
An admitted start runs for its configured duration from application time.
Stale unseen starts are dropped; repeating snapshots cannot replay completed
or cancelled runs. Rejected publication retires pending starts instead of
carrying them into a later snapshot. Stall, resume and lifecycle fail-off
invalidate the cancellation epoch, including starts accepted then overwritten
before the worker consumes them. A provider restart therefore cannot replay
a previous event: recovery needs a newer Trigger. No automatic publication
occurs while a gate is closed or after it resumes.
See [renderer runtime](renderer-runtime.md) for timing, overlap and fail-off.

### Fill effects

`LedActionSink::bind(ActionId, FillEffect)` binds an action to a
level-capable fill. A `FillEffect` names a configured `LedZone` (start,
length and fill direction; see `local_argb/lighting_zone.hpp`) and a colour.
The zone lights in proportion to the action's level, growing from its fill
direction: `StartToEnd`, `EndToStart` or `CenterOut`. One action may drive
several zones, and one action may drive both a fill and on/off effects.
`bind()` returns the same statuses as above. `DuplicateBinding` means the same
action already drives the same zone (in any colour), and `CapacityExceeded`
means `LedActionSink::kMaxFillBindings` (8) fill bindings exist.

The adapter does not validate the zone. The renderer draws nothing for an
empty, out-of-range or unknown-direction zone and caps each colour channel at
its brightness ceiling.

Bind every action before the engine attaches. `LedEffect` names strip regions,
not vehicle sides. A production profile maps vehicle signals to regions for
the physical installation; the current mapping is shown in the
[production configuration](../configuration/controller-config.md#production-lighting-profile).

## Commands

- `Activate` and `Deactivate` set the level of every binding of the action:
  on/off effects light or clear, fills become full or empty. Each command
  publishes the full effect state, not a delta.
- `SetLevel` sets the level of the action's fills and leaves its on/off
  effects unchanged. Levels are normalized to 0.0..1.0:
  - a level at or below 0.0, and NaN, is empty (fail-off);
  - a level at or above 1.0, including +infinity, is full;
  - a level in between is rounded to the nearest 1/65536.
- `Trigger` starts/restarts the action's bound transients. It leaves held
  on/off effects and fill levels unchanged. A Trigger with no transient binding
  publishes nothing, even if the action has held or fill bindings.
- Commands for unbound actions are ignored and publish nothing.

A command lists every non-empty fill in binding order, up to
`LightingFills::kCapacity` (8), and the priority of each fill and fixed
effect; see [Effect priority](#effect-priority). A command with fills does
not paint the generic compatibility colour. The fill list makes
`LightingCommand` larger, and the renderer queue still copies it by value; a
`static_assert` keeps it trivially copyable.

Before any transient has started, the engine's explicit initial `Deactivate`
therefore publishes a black baseline on each provider start.

While the engine is attached, the adapter must be the lighting sink's only
publisher. The production sink is a one-slot overwrite queue that accepts
publishes while started and its progress gate is open. A rejected publish is
not retried, and the engine never resends a deduplicated level, so a rejected
`Deactivate` can leave the effect lit. Renderer startup and gate behavior are
specified in [renderer runtime](renderer-runtime.md).

## Effect priority

Every binding takes an optional `EffectPriority`, a rank from 0 to 255
where higher wins. It is set per binding, never by effect type or signal:

- `bind(ActionId, LedEffect, EffectPriority)` sets the priority of an on/off
  binding. When several active bindings light one effect, the effect takes
  the highest of their priorities.
- `FillEffect::priority` sets the priority of a fill binding.
- `LightingTransient::priority` sets the priority of a transient binding.
- An unset priority is `EffectPriority::kDefault` (100) for every binding, so
  a configuration that sets none keeps the drawing order below.

The renderer resolves overlaps per physical LED. Each lit effect owns every
pixel of its region (the fill's zone, the brake region, or one turn region),
including the pixels its level or animation leaves dark, unless an effect of
higher priority also covers that pixel. No colours are blended or mixed.
Pixels that no other effect covers render exactly as they would alone.

Equal priorities resolve by drawing order, and the later effect wins: fills
in binding order (a later fill beats an earlier one, even on the same zone),
then brake, then the left turn, then the right turn, then active transients
in binding order. With the defaults a
fixed effect therefore wins over a fill it overlaps, and it also blanks the
fill's pixels its animation leaves dark. An empty fill, or one with an invalid
zone, owns nothing. A transient releases its owned pixels when it completes.

For example, a gauge fill over `20..79` at priority 50 and the right turn
(`65..99`) at the default priority: while the turn is lit it owns `65..79`,
and the gauge keeps rendering on `20..64`. Give the gauge priority 101 or
more and it owns `65..79` instead, while the turn keeps animating on
`80..99`.

## Fail-off policy: held level

Engine levels carry no deadline and are deduplicated, so the adapter publishes
commands containing held levels or latest transient starts with
`valid_until_us` set to the maximum timestamp. The renderer keeps held effects animating until a later command turns them off.
Transient duration belongs to the renderer and expires independently; a held
Deactivate neither extends nor cancels it. A command with no held effect, fill
or latest transient start is non-actionable and renders black. A snapshot
retaining only completed starts remains actionable but renders black; later
held updates do not replay those starts.

Loss of data fails off through the engine, not through a renderer deadline.
Every NoData, Stale or Unavailable reading becomes `Deactivate` (see
[action-engine.md](../action-engine.md#availability-policy)). A
`FreshnessUnverified` reading lights an effect only if its rule opts in with
`FreshnessRequirement::FreshOrUnverified`. Renderer startup black, driver
failure handling and the watched-dispatcher fail-off are specified in
[renderer runtime](renderer-runtime.md).

The composition root handles lifecycle gaps the engine cannot observe:
stopping the provider publishes no Unavailable notice, and
`ActionEngine::detach()` sends no `Deactivate`, so it calls
`local_argb::fail_off()` after stop or detach completes. If dispatcher notice
delivery stalls while the provider runs, no `Deactivate` can arrive; the root
watches dispatcher progress and the renderer fails off the held state. The
exact firmware wiring is documented in
[firmware composition](../../architecture/firmware-composition.md).

After a failed pixel write the renderer attempts black and holds its fault
until a later command follows a successful black write. Unlike the retired
legacy heartbeat path, held levels are not re-applied periodically, so the
strip remains dark until the next command. This behavior preserves fail-off
and is visible to the user.

## Evidence

- `tests/host/local_led_action_sink_tests.cpp` covers bindings, OR-ed effects,
  the explicit black baseline, ignored unbound Trigger/SetLevel commands, a
  rejected publish, and the binding errors. For fills it covers levels 0,
  0.25, 0.5 and 1.0, clamping of out-of-range and infinite levels, NaN
  fail-off, Deactivate, Activate, Trigger, fills published together with
  on/off effects, and the fill binding errors. It also covers default and
  configured binding priorities and the highest active priority of an effect.
  Transient coverage includes full state preservation, multiple zones per
  action, stable identities, changed restart sequences, clock/epoch stamping,
  held updates, ignored levels, finite duration and zone validation, physical
  duplicate zones and retired rejected starts.
- `components/local_argb/tests/rendering_tests.cpp` covers a fill under the
  brightness ceiling, the generic colour without fills, and the bounded fill
  list. For priority it covers the gauge and turn example in both binding
  orders, a turn owning its dark animation pixels, a fill above a turn,
  equal-priority overlaps between fills and between a fill and brake, and
  unchanged frames when no effects overlap.
- `tests/host/local_led_action_composition_tests.cpp` runs a make-independent
  fake provider, the engine, the adapter and the production
  `RendererController` into a fake `PixelFrameSink`. It shows left, right and
  hazard turn states rendered as pixels, a held effect with no deadline, and
  black for off, NoData, Stale, Unavailable and rejected unverified readings,
  plus recovery and the latched write fault. Event-rule coverage starts a
  finite transient, restarts it without queueing, restores an underlying fill
  on completion, prevents completed replay and preserves a start overwritten
  by a held update inside its admission window. It pins all-Deactivate black
  output after a completed Trigger, drops expired unseen starts, and exercises
  the real stall gate and fail-off with both running and accepted-but-unseen
  transients, including lifecycle mailbox overwrite and a later held update.
  It also renders a fill in every
  direction at levels 0, 0.25, 0.5 and 1.0, and drives one from an engine
  range rule on a generic numeric signal, which fails off on Stale data, and
  renders a higher-priority turn over a gauge fill and releases it. It stalls a fake dispatcher progress count under a held turn: the engine
  sends no `Deactivate`, black is commanded within the bound, a publish
  during the stall stays black, and after progress resumes a later notice
  lights the strip. An idle dispatcher keeps a held turn lit.
- `components/local_argb/tests/progress_fail_off_tests.cpp` covers the
  progress watchdog (bound, idle progress, wrap, one-shot stall and resume
  reports, re-detection, a backwards clock, re-arming), the publish gate,
  including a close that races a publish, and the shared fail-off policy
  (`ProgressFailOff`: close before black, resume without re-emitting, and
  transition counts reported once for logging). The telemetry service tests show
  `dispatch_progress()` advancing while idle, stopping in a blocked callback
  and advancing after it returns.
- The adapter dependency direction is maintained in
  [module boundaries](../../architecture/module-boundaries.md); host architecture
  checks are described in
  [architecture validation](../../development/architecture-validation.md).

## Lighting profile and RPM features

The shared lighting profile helper and RPM feature behavior are specified in
[controller lighting profile](../configuration/lighting-profile.md). This
document remains the authority for local LED sink bindings, commands, effect
priority, and held-level fail-off behavior.
