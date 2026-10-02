# Signal-provider dependency direction

The generic signal API sits above the Mazda telemetry facade. The dependency
direction runs one way:

```text
vehicle_signals                portable, value-only contracts and catalog view
  ^                            (only vehicle_core/telemetry_contracts.hpp from the core)
mazda/signal_provider.hpp      public provider: catalog(), read(), subscribe()
  ^                            (value-only; reaches no Mazda state or types)
Mazda facade and service       typed VehicleTelemetry, decoder, publication,
                               private catalog bindings and notification channels
```

- `lib/vehicle_signals` knows no vehicle make. Signals are identified by
  canonical string keys (for example `vehicle.engine_rpm`), and enum values by
  choice keys (for example `left`). Numeric `SignalId`s are runtime handles for
  one build and must not be persisted.
- `MazdaSignalProvider` exposes the Mazda-owned signal catalog. Reads use the
  same coherent publication as the typed polling API. Subscriptions share the
  typed notification channels and their capacity, defined by
  `vehicle_core::kNotificationSubscribersPerChannel`. Subscriptions are
  stopped-only mutations on the facade's lifecycle owner. The facade owns
  every registration, typed or generic, so the provider is a stateless view
  that is safe to destroy at any time, including while the facade runs.
- The typed `mazda::VehicleTelemetry` API and local ARGB behavior remain
  separate from the generic provider contract.
- `vehicle_signals::SignalProvider` is the generic provider port that
  `MazdaSignalProvider` implements (`catalog()`, `read()`, `subscribe()`,
  `unsubscribe()`). The generic action engine consumes only this port; see
  [`action-engine.md`](../specs/action-engine.md).

## Brake signal contract

The Mazda generic catalog contains nineteen signals. `vehicle.brake_pressed`
is a Boolean signal with Read and Notify capabilities and Confirmed field
interpretation evidence. Reads and notifications use the same coherent brake
publication; generic subscribers share the public
`on_brake_pressed_changed` notification channel. Its capacity is
`vehicle_core::kNotificationSubscribersPerChannel`, shared across typed and
generic registrations. Typed subscribers can exhaust the capacity needed by
generic `subscribe()` calls, and generic subscribers can exhaust the capacity
needed by typed subscriptions.

Default brake freshness remains unset. A valid decoded observation is
`FreshnessUnverified` until a caller explicitly supplies a freshness policy;
Confirmed evidence does not imply `Fresh` availability. Unknown, malformed,
stopped and faulted brake observations remain unavailable through the public
contracts. Controller configurations require `fresh` for the canonical brake
key: both the YAML compiler and persisted C++ validation reject
`fresh_or_unverified` before applying any output. There is no owner-approved
opt-in for unverified brake observations. The default controller profile
binds no brake-pedal action to an LED output. See
[signal evidence](../protocol/signal-evidence.md) for the field mapping and its evidence boundary.

## Boundary contracts

`vehicle_signals` depends on `vehicle_core` value contracts and has no Mazda
dependency. The provider is a value-only public port; it does not expose the
Mazda service, publication store, private catalog, Mazda state/decoder types,
or driver and RTOS headers. The action engine depends on the provider port,
not the Mazda implementation. The repository's architecture and public-header
checks enforce these boundaries; see
[`architecture-validation.md`](../development/architecture-validation.md)
for the checks and their invocation.

The generic consumer is exercised through the production runtime and Mazda
publication path in `tests/host/signal_consumer_tests.cpp`. It compares key-
based reads and notifications with the typed API, including enum values and
malformed-frame, transport-silence, and transport-fault behavior. A catalog
sweep covers the Mazda provider's rows.
