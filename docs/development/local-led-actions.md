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

### Fill effects

`LedActionSink::bind(ActionId, FillEffect)` binds an action to a
level-capable fill (#29). A `FillEffect` names a configured `LedZone` (start,
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

Bind every action before the engine attaches. `LedEffect` names strip
regions, not vehicle sides: the WeAct strip is mounted mirrored, so the
firmware binds the vehicle's left turn to `RightTurn`, as the legacy binding
did.

## Commands

- `Activate` and `Deactivate` set the level of every binding of the action:
  on/off effects light or clear, fills become full or empty. Each command
  publishes the full effect state, not a delta.
- `SetLevel` sets the level of the action's fills and leaves its on/off
  effects unchanged. Levels are normalized to 0.0..1.0:
  - a level at or below 0.0, and NaN, is empty (fail-off);
  - a level at or above 1.0, including +infinity, is full;
  - a level in between is rounded to the nearest 1/65536.
- `Trigger` is ignored. It carries no level.
- Commands for unbound actions are ignored and publish nothing.

A command lists every non-empty fill in binding order, up to
`LightingFills::kCapacity` (8), and the priority of each fill and fixed
effect; see [Effect priority](#effect-priority). A command with fills does
not paint the generic compatibility colour. The fill list makes `LightingCommand` larger, and the
renderer queue still copies it by value; a `static_assert` keeps it trivially
copyable.

The engine's explicit initial `Deactivate` therefore publishes a black
baseline on each provider start.

While the engine is attached, the adapter must be the lighting sink's only
publisher, and the sink must accept every publish. The production sink is a
one-slot overwrite queue that rejects only before `local_argb::start()`, so
start the renderer before the engine attaches. A rejected publish is not
retried, and the engine never resends a deduplicated level, so a rejected
`Deactivate` would leave the effect lit.

## Effect priority

Every binding takes an optional `EffectPriority` (#30), a rank from 0 to 255
where higher wins. It is set per binding, never by effect type or signal:

- `bind(ActionId, LedEffect, EffectPriority)` sets the priority of an on/off
  binding. When several active bindings light one effect, the effect takes
  the highest of their priorities.
- `FillEffect::priority` sets the priority of a fill binding.
- An unset priority is `EffectPriority::kDefault` (100) for every binding, so
  a configuration that sets none keeps the drawing order below.

The renderer resolves overlaps per physical LED. Each lit effect owns every
pixel of its region (the fill's zone, the brake region, or one turn region),
including the pixels its level or animation leaves dark, unless an effect of
higher priority also covers that pixel. No colours are blended or mixed.
Pixels that no other effect covers render exactly as they would alone.

Equal priorities resolve by drawing order, and the later effect wins: fills
in binding order (a later fill beats an earlier one, even on the same zone),
then brake, then the left turn, then the right turn. With the defaults a
fixed effect therefore wins over a fill it overlaps, as before #30; the
difference is that it now also blanks the fill's pixels its animation leaves
dark. An empty fill, or one with an invalid zone, owns nothing.

For example, a gauge fill over `20..79` at priority 50 and the right turn
(`65..99`) at the default priority: while the turn is lit it owns `65..79`,
and the gauge keeps rendering on `20..64`. Give the gauge priority 101 or
more and it owns `65..79` instead, while the turn keeps animating on
`80..99`.

## Fail-off policy: held level

Engine levels carry no deadline and are deduplicated, so the adapter publishes
every lit command as *held*: `valid_until_us` is the maximum timestamp and the
renderer keeps animating until a later command turns the effect off. A
command with no lit effect and no fill is non-actionable and renders black.

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
- If notice delivery stops while the provider runs (a hung dispatcher), no
  `Deactivate` can arrive, so the renderer bounds it instead (#34). The
  telemetry facade counts completed dispatcher loop passes
  (`VehicleTelemetry::dispatch_progress()`). The count advances on idle passes
  too, so stable vehicle state with no notices is healthy; it stops only
  while the dispatcher is stuck, for example in a callback. The composition
  root registers the count with `local_argb::watch_progress()`, and the
  renderer's existing `argb_guard` supervisor samples it on its 10 ms poll.
  If the count does not change for `kProgressStallFailOffUs` (2 s), the
  supervisor closes the renderer queue to lighting publishers and queues
  black past the closed gate, so black is commanded within
  `kProgressFailOffBoundUs` (2 s plus two polls) of the last dispatcher
  pass; the worker renders it on its next pass, a few ms later. The
  supervisor only counts stalls and resumes; the lower-priority worker logs
  them, so the small guard stack never formats log output. While closed, a publish
  from another context, such as a polled `SetLevel`, is rejected, and a
  publish that races the close is followed by black. The fault does not
  latch: once the count changes again the queue reopens, and the next
  command lights the strip. Nothing is re-emitted, so the strip stays black
  until then. The engine and this adapter know nothing about the watch.

Write-fault recovery also differs from the legacy binding. After a failed
pixel write the renderer writes black and latches its fault until the next
command. The legacy heartbeat re-applied its command every 100 ms and so
relit the strip quickly. With held levels, the strip stays dark until the next
level change. This is fail-safe but visible.

## Evidence

- `tests/host/local_led_action_sink_tests.cpp` covers bindings, OR-ed effects,
  the explicit black baseline, ignored Trigger/SetLevel/unbound commands, a
  rejected publish, and the binding errors. For fills it covers levels 0,
  0.25, 0.5 and 1.0, clamping of out-of-range and infinite levels, NaN
  fail-off, Deactivate, Activate, Trigger, fills published together with
  on/off effects, and the fill binding errors. It also covers default and
  configured binding priorities and the highest active priority of an effect.
- `components/local_argb/tests/rendering_tests.cpp` covers a fill under the
  brightness ceiling, the generic colour without fills, and the bounded fill
  list. For priority it covers the #30 gauge and turn example in both binding
  orders, a turn owning its dark animation pixels, a fill above a turn,
  equal-priority overlaps between fills and between a fill and brake, and
  unchanged frames when no effects overlap.
- `tests/host/local_led_action_composition_tests.cpp` runs a make-independent
  fake provider, the engine, the adapter and the production
  `RendererController` into a fake `PixelFrameSink`. It shows left, right and
  hazard turn states rendered as pixels, a held effect with no deadline, and
  black for off, NoData, Stale, Unavailable and rejected unverified readings,
  plus recovery and the latched write fault. It also renders a fill in every
  direction at levels 0, 0.25, 0.5 and 1.0, and drives one from an engine
  range rule on a generic numeric signal, which fails off on Stale data, and
  renders a higher-priority turn over a gauge fill and releases it. For #34
  it stalls a fake dispatcher progress count under a held turn: the engine
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
- `architecture_contracts` restricts the adapter to the engine action port,
  the renderer sink contract, core time values and standard headers. It
  rejects any provider, Mazda, CAN, LED driver, RTOS/SDK or WLED dependency,
  a `subscribe` call, any other link target, include directory or ESP-IDF
  requirement (including `${COMPONENT_LIB}`), and directory-scope
  `include_directories()` or `link_libraries()`.

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
3. applies the RPM level fill through `controller_config::apply()`: it binds
   a `FillEffect` over the whole strip (`CenterOut`, priority 50, so the turn
   effects at the default 100 draw over it) and adds a range rule on
   `vehicle.engine_rpm`. See [RPM level fill](#rpm-level-fill);
4. registers the typed turn notice log and attaches the engine. These are the
   turn channel's two subscriber slots;
5. registers the facade's dispatcher progress with
   `local_argb::watch_progress()`, so a stalled dispatcher fails the strip
   off (see [Fail-off policy: held level](#fail-off-policy-held-level));
6. starts the facade, then calls `engine.sample_polled_rules()` on every
   100 ms pass of its runtime loop.

Any setup failure calls `local_argb::fail_off()` and refuses to start CAN.
The application never stops the facade or detaches the engine.

### RPM level fill

`components/controller_config` owns the feature: which signal drives the fill,
over which input range, and with which freshness requirement. Neither the
engine nor this adapter learns about RPM; the fill only receives a 0.0..1.0
`SetLevel`.

- `controller_config::RpmLevelFillConfig` holds the input range
  (`RpmRange{min_rpm, max_rpm}`, default 0..6500 rpm), the `ActionId` and the
  `FillEffect`. `min_rpm` and below is an empty fill, `max_rpm` and above a
  full fill, and speeds in between fill linearly. Changing the range changes
  the mapping without any renderer change.
- `range_rule()` builds the polled range rule. It uses `FreshOrUnverified`,
  because the Mazda provider reports RPM as `FreshnessUnverified` and has no
  RPM freshness timeout. Missing, stale or unavailable readings still empty
  the fill.
- `apply()` binds the fill, then adds the rule. A failed binding adds no rule;
  a rejected rule, such as an empty or inverted range (`InvalidRange`), leaves
  the binding, so firmware treats any failure as fatal setup and fails off.

The turn rules in `main.cpp` keep the strict `Fresh` requirement, and
`tools/validate_local_argb_boundary.py` still forbids `FreshOrUnverified` and
direct `add_range_rule()` calls there. `tests/host/rpm_level_fill_tests.cpp`
covers the defaults, endpoints, linear midpoints, clamping, a configured
range, unverified and missing readings, and both failure paths.

This step is where the documented differences from the legacy binding reach
the hardware: a write fault stays dark until the next level change. A hung
dispatcher no longer holds the strip lit: it fails off within about 2 s (#34). See
[Fail-off policy: held level](#fail-off-policy-held-level).

The migration keeps the visible turn and hazard behavior of the legacy
`mazda::application::bind_local_argb_sink()` binding. That binding also lit
brake on a Fresh brake reading. Brake has no freshness timeout, so it is never
Fresh and never lit. Brake is also not in the generic catalog, so the
migration drops it with no visible change.

The legacy binding is no longer bound in firmware. The renderer queue has one
slot, so the LED sink must be its only publisher. The binding stays compiled
in `mazda_telemetry` as a rollout fallback and is to be removed after rollout.
The local ARGB boundary validator rejects it in the vehicle application. It
checks `main.cpp` with comments and string contents removed, so neither a
comment nor a log message can stand in for a call. It requires the wiring
above:

- the components;
- the startup calls inside `app_main`, with attach before start. A call that
  `app_main` does not make fails the check;
- `fail_off()` in the block of every `return` between `local_argb::start()`
  and `telemetry.start()`, after the last `case` or `default` label, and on
  the success path after any `telemetry.stop()` or `engine.detach()`, before
  any return;
- exactly the mirrored `kTurnRules` and `kEffectBindings` entries, applied
  by the only bind and rule loops, with no `continue` or `break`;
- no `FreshOrUnverified`.

`tests/tools/validate_local_argb_boundary_test.py` covers these rules,
including braceless, commented-out, nested, decoy-string, switch-label and
helper-wrapped bypasses.

Hardware follow-up: engine evaluation and the LED publish now run on the
4 KiB `mazda_notify` dispatcher stack. On a bench with debug logging
enabled, check that task's `uxTaskGetStackHighWaterMark()`.
