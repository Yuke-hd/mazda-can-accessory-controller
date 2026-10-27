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

The Mazda generic catalog contains twenty-five signals. `vehicle.brake_pressed`
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
contracts. Controller configurations default to `fresh`, which excludes
unverified brake observations. The project owner explicitly approved the
`fresh_or_unverified` opt-in for Boolean brake rules. With an unset brake
timeout, the last decoded value can remain eligible when brake frames stop
arriving while other CAN traffic keeps transport health available; accepting
this policy does not promote the reading to `Fresh` or establish timing
evidence. The factory controller profile uses this opt-in for its `brake`
action, bound to the LED `Brake` effect above the RPM red zone. See
[signal evidence](../protocol/signal-evidence.md) for the field mapping and its
evidence boundary, and [controller configuration](../specs/configuration/controller-config.md#yaml-compiler)
for the owner-approved opt-in.

## Acceleration signal contract

`vehicle.acceleration.longitudinal` and `vehicle.acceleration.lateral` are
Number signals with Read capability and
`vehicle_signals::SignalUnit::MetresPerSecondSquared`. Values retain the
source decoder's SI magnitude and sign without conversion to g. Runtime ids
20 and 21 append these entries; the existing nineteen ids and keys retain
their assignments. The generic API exposes no CAN coordinates, Mazda state,
or decoder types, and does not provide acceleration notifications.

Both readings use the coherent publication's polling descriptors and preserve
source value presence, availability, and Reference confidence, including
NoData and Unavailable states. The default policy assigns both readings a
250 ms freshness timeout; a valid observation is `Fresh` until that timeout
expires. The candidate axis interpretation and source timing evidence remain
unconfirmed; see [signal evidence](../protocol/signal-evidence.md).

The pinned core 0.1.0 has no acceleration unit enumerator. Its internal source
signals use `None` with SI member names; the Mazda-owned generic catalog
supplies the engineering unit without modifying that dependency or rescaling
values. The controller compiler's catalog manifest includes both Read-only
numeric keys for sampled conditions and numeric range rules.

`vehicle.accelerator_pedal_position` is a Number/Read-only signal with
`SignalUnit::None` because the shared core has no percentage unit. The Mazda
decoder validates the source percentage range `[0, 100]` before converting it
to the normalized public range `[0, 1]`; runtime id 22 appends after the
existing entries. Its DBC mapping remains Reference confidence, has no default
freshness timeout, and is available to sampled and normalized range rules
through the controller catalog manifest.

`vehicle.brake_pressure` is a Number/Read-only signal with generic
`SignalUnit::None`; consumers interpret its value as bar. It uses runtime id
23 and the owner-reported Confirmed 17-bit `0x078` mapping. Values below zero
are unavailable without clamping, while the 91.071 value is only the encoding
ceiling. Its default freshness timeout remains unset, so valid readings are
`FreshnessUnverified`; replay writes JSON `null` for the generic unit and BLE
exports the `none` token.

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

## Blind-spot signal contract

`vehicle.blind_spot.left` and `vehicle.blind_spot.right` append runtime IDs
24 and 25. Both are Enum Read + Notify signals with stable choices `unknown`,
`clear`, `detected`, `warning`, sharing bounded typed/generic notification
channels. Confidence is Reference and default freshness is unset; valid values
are FreshnessUnverified. Reserved and disabled states are unavailable;
[signal evidence](../protocol/signal-evidence.md#blind-spot-candidates-bsm-0x477)
records the conservative policy. No LED work occurs in telemetry.

Before the first accepted value, invalid/disabled observations retain NoData;
after a value has been accepted, invalidation reports Unavailable while
retaining that historical value. Both states fail off.
