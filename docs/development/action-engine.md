# Generic action engine

`lib/action_engine` (#9) turns generic signal notices into action commands.
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
  is the minimal provider interface: `catalog()`, `subscribe()` and
  `unsubscribe()`. It keeps the callback contract from `signal_contracts.hpp`:
  subscriptions change only while the provider is stopped, callback contexts
  are borrowed, and callbacks must not call back into the provider. A provider
  also delivers notifications serially from one dispatcher context.
  `mazda::MazdaSignalProvider` implements it. `tests/support/fake_signal_provider.hpp`
  is the host fake. The port lives in `vehicle_signals` so that the Mazda
  component never depends on the engine.
- `action_engine::ActionSink` receives `ActionCommand{ActionId, kind}`.
  `kind` is `Activate`, `Deactivate` or `Trigger`. An `ActionId` is a
  configured number; zero is invalid. Its meaning belongs to the sinks. Every
  registered sink receives every command and ignores the ids it does not
  handle. A sink owns its own bounded queueing and overflow behavior.

## Configuration

Rules use the persisted form: signal keys and enum choice keys, never numeric
ids. A `SignalCondition` is `{signal_key, Comparison, RuleOperand}`, and the
operand is `boolean(bool)`, `number(float)` or `choice(key)`. For example,
`transmission.gear Equal choice("reverse")`.

`add_state_rule()` and `add_event_rule()` resolve a condition through
`provider.catalog()` when the rule is added. The runtime rule keeps only a
`SignalId` and a compact `SignalValue`. A rule is rejected with:

| Status | Cause |
| --- | --- |
| `InvalidState` | The engine is attached. |
| `InvalidAction` | The `ActionId` is zero. |
| `UnknownSignal` | The key is not in the catalog. |
| `UnsupportedCapability` | The signal cannot notify. |
| `TypeMismatch` | The operand type differs from the signal type. |
| `InvalidOperand` | A Number operand is NaN or infinite. |
| `UnsupportedComparison` | An ordered comparison is used on a Boolean or Enum signal. |
| `UnknownChoice` | The choice key is not a choice of the Enum signal. |
| `CapacityExceeded` | 16 rules (or 4 sinks) are already registered. |

## Availability policy

Each rule declares a `FreshnessRequirement`. The default, `Fresh`, is strict.
`FreshOrUnverified` also accepts `FreshnessUnverified`. A reading is
*actionable* only if all of these hold:

- it has a value of the signal's type, and a Number value is finite;
- its availability meets the rule's requirement.

`NoData`, `Stale` and `Unavailable` are never actionable. A state rule treats
any non-actionable reading as off and emits `Deactivate`. This fail-off
behavior is fixed.

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

`attach()` resets every rule. After it, a state rule's output is unknown and
an event rule has no baseline.

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
- The engine does not lock. It relies on the provider to deliver notices
  serially. Commands reach sinks on the provider's dispatcher context, in rule
  order and then in sink registration order. Evaluation uses fixed arrays and
  never allocates.

## Evidence

- `tests/host/action_engine_*_tests.cpp` run the fake provider, the engine and
  a recording sink on the host. They cover resolution errors, both freshness
  policies, initial/fault/recovery/coalesced notices, deduplication, shared
  subscriptions, capacities, setup only while detached, attach rollback and
  retryable detach.
- `tests/host/action_engine_composition_tests.cpp` is the only code that
  knows both the engine and Mazda. It drives real turn-signal frames through
  `MazdaSignalProvider` into the engine.
- `architecture_contracts` builds `action_engine` against only `vehicle_core`
  and `vehicle_signals`. It checks the resolved link targets and include
  directories, the dependency closure of the probe and of every engine source
  file, and scans the sources for Mazda keys and types and for WLED, ARGB,
  TWAI, `can_bus` or FreeRTOS names. `public_header_boundary` compiles each
  engine header in isolation and forbids any Mazda or output-adapter header.

Out of scope for #9: numeric range mapping and a provider `read()` (#10),
firmware wiring and the local LED sink (#11), and WLED transport (#12).
