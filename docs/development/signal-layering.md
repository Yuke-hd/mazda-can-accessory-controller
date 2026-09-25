# Generic signal layering

The first signal-abstraction milestone (#2) adds one generic signal API on
top of the Mazda telemetry facade. The dependency direction runs one way:

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
- `MazdaSignalProvider` exposes the Mazda-owned 18-row catalog. Reads use the
  same coherent publication as the typed polling API. Subscriptions share the
  typed notification channels and their two-slot capacity. Subscriptions are
  stopped-only mutations on the facade's lifecycle owner. The facade owns
  every registration, typed or generic, so the provider is a stateless view
  that is safe to destroy at any time, including while the facade runs.
- The typed `mazda::VehicleTelemetry` API and local ARGB behavior are
  unchanged.

## Enforced boundaries

| Gate | What it proves |
| --- | --- |
| `architecture_contracts` | `vehicle_signals` builds against only the core's `vehicle_core` component. It has no Mazda include directories or link targets, and its compiler dependency closure reaches only standard headers and `vehicle_core/telemetry_contracts.hpp`. Its sources hold no Mazda key literal or type. The host generic consumer includes only the provider and `vehicle_signals` headers. |
| `public_header_boundary` | `mazda/signal_provider.hpp` and both `vehicle_signals` headers are compiled in isolation. The provider may not reach the service, publication store, private catalog, `mazda/state.hpp`, `mazda/types.hpp`, `mazda/definitions.hpp`, or driver/RTOS headers. A provider-only CMake consumer repeats this under the real target's compile flags. |
| `architecture_checker_regression`, `public_header_checker_regression` | Each rule fails on a planted violation in a temporary fixture. |

## Parity evidence

`tests/host/signal_consumer_tests.cpp` drives generated frames through the
production runtime, Mazda decoder and publication path, using a fake clock and
the host acquisition source. The consumer in
`tests/host/generic_signal_consumer.*` resolves `vehicle.engine_rpm` and
`vehicle.turn_state` by key, reads RPM, subscribes to turn state and decodes
choice keys. The tests compare it with the typed API: RPM reads, turn notices
(value, availability, validation and all four flags), a request versus a lamp
from different frames, an enum, and malformed-frame, transport-silence and
transport-fault behavior. A sweep checks all 18 catalog rows through the
public provider.
