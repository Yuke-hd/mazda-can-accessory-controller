# Generic action engine

`lib/action_engine` (#9, #10, #32) turns generic signal notices and sampled
readings into action commands.
It knows no vehicle make, CAN frame, driver, RTOS, LED, WLED transport or
persistence format. The only dependency is `vehicle_signals`.

```text
application composition        the only code that knows both sides
  |           |
  v           v
ActionEngine  MazdaSignalProvider (or a host fake)
  |           |
  +--> vehicle_signals::SignalProvider   generic provider port
  +--> action_engine::ActionSink         generic output port
```

## Ports

- `vehicle_signals::SignalProvider` (`vehicle_signals/signal_provider.hpp`)
  is the minimal provider interface: `catalog()`, `read()`, `subscribe()` and
  `unsubscribe()`. `read()` returns the latest state and is safe from any
  context. A request failure (`InvalidSignal`, `UnsupportedCapability`, or a
  provider fault such as `Timeout`) is distinct from a successful reading
  whose availability is `NoData`, `Stale` or `Unavailable`. It keeps the callback contract from `signal_contracts.hpp`:
  subscriptions change only while the provider is stopped, callback contexts
  are borrowed, and callbacks must not call back into the provider. A provider
  also delivers notifications serially from one dispatcher context.
  `mazda::MazdaSignalProvider` implements it. `tests/support/fake_signal_provider.hpp`
  is the host fake. The port lives in `vehicle_signals` so that the Mazda
  component never depends on the engine.
- `action_engine::ActionSink` receives `ActionCommand{ActionId, kind, level}`.
  `kind` is `Activate`, `Deactivate`, `Trigger` or `SetLevel`. `level` is
  meaningful only for `SetLevel` (always finite) and is 0 otherwise; command
  equality includes it. An `ActionId` is a
  configured number; zero is invalid. Its meaning belongs to the sinks. Every
  registered sink receives every command and ignores the ids it does not
  handle. A sink owns its own bounded queueing and overflow behavior. The
  engine serializes `execute()` calls, but a call may run on the provider's
  dispatcher context (notified rules) or on the context that samples polled
  rules.

## Configuration

Rules use the persisted form: signal keys and enum choice keys, never numeric
ids. A `SignalCondition` is `{signal_key, Comparison, RuleOperand}`, and the
operand is `boolean(bool)`, `number(float)` or `choice(key)`. For example,
`transmission.gear Equal choice("reverse")`.

`add_state_rule()`, `add_event_rule()` and `add_sampled_state_rule()` resolve
a condition through `provider.catalog()` when the rule is added. The runtime rule keeps only a
`SignalId` and a compact `SignalValue`. A rule is rejected with:

| Status | Cause |
| --- | --- |
| `InvalidState` | The engine is attached. |
| `InvalidAction` | The `ActionId` is zero. |
| `DuplicateAction` | A level rule (state, range or sampled state) already drives this `ActionId` (level rules only). |
| `UnknownSignal` | The key is not in the catalog. |
| `UnsupportedCapability` | The signal cannot notify (state and event rules) or be read (range and sampled state rules). |
| `TypeMismatch` | The operand type differs from the signal type. |
| `InvalidOperand` | A Number operand is NaN or infinite. |
| `UnsupportedComparison` | An ordered comparison is used on a Boolean or Enum signal. |
| `UnknownChoice` | The choice key is not a choice of the Enum signal. |
| `InvalidRange` | Range rules only: a range bound or span is not finite, or `input.from >= input.to`. |
| `CapacityExceeded` | 16 state/event rules, 8 polled (range plus sampled state) rules, or 4 sinks are already registered. |

A range rule's signal must be a Number (`TypeMismatch` otherwise). Its checks
run in the order `InvalidState`, `InvalidAction`, `DuplicateAction`,
`UnknownSignal`, `UnsupportedCapability`, `TypeMismatch`, `InvalidRange`,
`CapacityExceeded`. A sampled state rule's checks run in the order
`InvalidState`, `InvalidAction`, `DuplicateAction`, `UnknownSignal`,
`UnsupportedCapability`, `TypeMismatch`, `InvalidOperand`,
`UnsupportedComparison`, `UnknownChoice`, `CapacityExceeded`.

`add_sink()` returns `InvalidState` while attached, `DuplicateSink` for a sink
that is already registered (it would otherwise receive every command twice),
and `CapacityExceeded` after four sinks.

State, range and sampled state rules are level outputs, so each owns its
`ActionId`. With
two level rules on one action, the sink's final level would depend on rule
order and on which signal changed last. Event rules emit one-shot `Trigger`s
and may share an `ActionId` with each other and with one level rule.

## Availability policy

Each rule declares a `FreshnessRequirement`. The default, `Fresh`, is strict.
`FreshOrUnverified` also accepts `FreshnessUnverified`. A reading is
*actionable* only if all of these hold:

- it has a value of the signal's type, and a Number value is finite;
- its availability meets the rule's requirement.

`NoData`, `Stale` and `Unavailable` are never actionable. State, range and
sampled state rules treat any non-actionable reading, and the polled (range
and sampled state) rules also treat a failed read, as off and emit
`Deactivate`. This fail-off behavior is fixed.

Choose the policy per signal. For example, Mazda configures no freshness
timeout for engine RPM, so its readings are `FreshnessUnverified`: an RPM
range rule with the strict default stays off, and a gauge has to opt in to
`FreshOrUnverified`. The composition test demonstrates both.

## Rule semantics

Notices carry the latest state only. Transitions merged away by provider
coalescing are never reconstructed.

| Notice | State rule (`Activate` while actionable and the condition holds) | Event rule (`Trigger` on a transition in the chosen direction) |
| --- | --- | --- |
| `initial` | Always emits the explicit `Activate` or `Deactivate`, even if unchanged. This sets the sink baseline on each provider start. | Sets the baseline. Never triggers. |
| ordinary | Emits only when the output changes (deduplicated). | Triggers when two consecutive actionable readings differ in the chosen direction (`BecomesTrue` or `BecomesFalse`). |
| non-actionable (`NoData`, `Stale`, `Unavailable`, rejected `FreshnessUnverified`) | `Deactivate` if it was not already off. | Clears the baseline. Never triggers. |
| `became_unavailable` | `Deactivate`, because the reading is not actionable. | Data gap: rebaselines from the current reading if it is actionable. Never triggers. |
| `recovered` | Re-evaluates. `Activate` if the condition holds. | Data gap: rebaselines. Never triggers. |
| `coalesced` | Evaluates the latest state only. | Compares the latest state with the baseline: at most one `Trigger`. Edges merged by coalescing are lost. |

`attach()` resets every rule. After it, a state rule's output is unknown, an
event rule has no baseline, and a polled rule has emitted nothing.

## Range rules

A `RangeRuleConfig` is `{signal_key, input, output, action, freshness}`, where
`input` and `output` are `NumericRange{from, to}`. For example,
`engine.rpm 0..6500 -> 0..1` drives a tachometer progress level.

- Mapping: `x <= input.from` gives exactly `output.from`; `x >= input.to`
  gives exactly `output.to`; values in between are interpolated linearly. The
  input must be ascending. The output may be ascending, descending (an inverse
  mapping) or equal (a constant level).
- Cadence: range rules are polled; see [Polled sampling](#polled-sampling).
- Output: `SetLevel(level)` when the level differs from the last emitted
  command, or when nothing was emitted since `attach()`. Unchanged levels,
  including different inputs clamped to the same endpoint, are not repeated.
  A failed read or a non-actionable reading emits `Deactivate` once, and the
  next actionable reading emits `SetLevel` again.

## Sampled state rules

A `SampledStateRuleConfig` is `{condition, action, freshness}`: the same
`SignalCondition` as a state rule, evaluated on sampled reads of a
Read-capable signal instead of notices. For example,
`engine.rpm Greater number(3000)` activates a shift light above 3000 RPM and
deactivates it at or below 3000. All six comparisons are accepted; ordered
comparisons need a Number signal, as for state rules. Compound AND/OR
conditions are not supported.

- Output: the rule is on when the read succeeds, the reading is actionable and
  the condition holds. It emits `Activate` or `Deactivate` only when that
  output changes. After `attach()` the output is unknown, so the first sample
  always emits the explicit baseline, including `Deactivate` for a
  non-actionable first reading.
- Fail-off: a failed read or a non-actionable reading emits `Deactivate` once;
  the next actionable reading re-evaluates the condition.

## Polled sampling

Range and sampled state rules are *polled rules*: they are sampled, not
notified, and share one capacity of 8 (`ActionEngine::kMaxPolledRules`).

- The application calls `ActionEngine::sample_polled_rules()` at its own
  cadence. Each call reads every polled rule's signal, in insertion order,
  through `SignalProvider::read()`, and passes the result to that rule. This
  suits Read-only signals such as RPM. The signal layer has no polling worker
  and no synthetic notification. Polled rules need no subscription.
- `sample_polled_rules()` returns `InvalidState` while detached and `Ok`
  otherwise; a failed read is handled per rule, not returned.
- Each rule reads its own signal, so two rules on the same signal may see
  different samples within one call.
- The WeAct firmware calls it every 100 ms from its runtime loop. Its RPM
  level fill is configured in `components/controller_config`; see
  [local-led-actions.md](local-led-actions.md#rpm-level-fill).

## Lifecycle

- Choose the provider when the engine is constructed; it cannot be replaced.
  Add sinks and rules only while the engine is detached.
- `attach()` and `detach()` change provider subscriptions. Call them on the
  provider's lifecycle owner while the provider is stopped.
- `attach()` subscribes once per distinct `SignalId`. Rules on the same signal
  share that subscription, because Mazda channels have only two subscriber
  slots. If a subscribe fails, `attach()` removes the subscriptions it made
  and returns the provider's status.
- `detach()` removes every subscription. If the provider rejects a removal,
  `detach()` keeps the remaining subscriptions, stays attached and returns the
  failure, and the caller can retry. Engine-local misuse, such as a second
  `attach()`, returns `InvalidState`.
- The engine is a borrowed callback context. Detach it before destroying it,
  unless the provider is destroyed first. The provider and every sink must
  outlive the attachment.
- The provider delivers notices serially on its dispatcher context, and
  `sample_polled_rules()` runs on the caller's context. The engine serializes
  all rule evaluation and sink delivery with one `std::mutex`, so sinks never
  see concurrent `execute()` calls. Provider reads happen before the lock is
  taken, so a read never waits on a sink. Call `sample_polled_rules()` from one
  context at a time, and never concurrently with configuration, `attach()` or
  `detach()`. Commands reach sinks in rule order and then in sink
  registration order. Evaluation uses fixed arrays and never allocates.

## Evidence

- `tests/host/action_engine_*_tests.cpp` run the fake provider, the engine and
  a recording sink on the host. They cover resolution errors, both freshness
  policies, initial/fault/recovery/coalesced notices, deduplication, shared
  subscriptions, capacities, setup only while detached, attach rollback and
  retryable detach. The range tests map 0..6500 RPM to 0..1, including
  endpoints, out-of-range values, NoData, Stale, Unavailable, read failures
  and recovery, and check that sampling on one thread and notices on another
  never overlap sink calls. The sampled state tests cover `rpm > 3000` on and
  off transitions, every comparison at its boundary, deduplication, NoData,
  Stale, Unavailable, failed reads and recovery, both freshness policies, the
  shared level-action namespace and polled capacity, and range and sampled
  state rules on one signal.
- `tests/host/action_engine_composition_tests.cpp` is the only code that
  knows both the engine and Mazda. It drives real turn-signal frames through
  `MazdaSignalProvider` into the engine, and samples a real engine-RPM frame
  into a `SetLevel`.
- `architecture_contracts` builds `action_engine` against only `vehicle_core`
  and `vehicle_signals`. It checks the resolved link targets and include
  directories, the dependency closure of the probe and of every engine source
  file, and scans the sources for Mazda keys and types and for WLED, ARGB,
  TWAI, `can_bus` or FreeRTOS names. `public_header_boundary` compiles each
  engine header in isolation and forbids any Mazda or output-adapter header.

The local LED sink and its firmware wiring are documented in
[local-led-actions.md](local-led-actions.md). Out of scope: WLED transport
(#12).
