# S1-A/S1.5 module and header boundaries

The S1-A layout separates portable transport primitives from Mazda semantics.
The S1.5 boundary handoff then makes the corrected public-header boundary a
required host regression gate before Stage 2 adds publication, task, lighting,
or application consumers.

## Ownership map

| Module | Public entry points | Owns | Boundary rule |
| --- | --- | --- | --- |
| `vehicle_core` | `vehicle_core/vehicle_core.hpp`, `time.hpp`, `frame.hpp`, `signal.hpp`, `telemetry_contracts.hpp`, `reading.hpp`, `notification.hpp` | Portable time/frame/signal primitives and value-copy reading/notification contracts | No Mazda model definitions, decoders, CAN driver, board, ESP-IDF, or RTOS dependency |
| `mazda` semantic layer | `mazda/types.hpp`, `freshness.hpp`, `state.hpp`, `definitions.hpp`, `decoder.hpp` | Mazda enums, freshness policy, state, capture-derived definitions, and pure decoder APIs | Lower-level APIs may use frames and decoder-health contracts; they are not façade dependencies |
| `mazda` façade layer | `mazda/facade_contracts.hpp`, `reading.hpp`, `notification.hpp`, `availability.hpp`, `telemetry_contracts.hpp` | Application result/configuration, polling, notification, diagnostics, value aliases, and the lower-level availability evaluator | `facade_contracts.hpp`, `vehicle_telemetry.hpp`, and the compatibility umbrella remain free of frame, decoder, mutable signal/state, lighting, CAN-driver, board, and RTOS headers |
| `mazda` implementation boundary | `lib/mazda/internal_include/mazda/internal_contracts.hpp` | Decoder/service handoff values containing raw frames and health observations | Never exported through the public include root; implementation and explicitly authorized tests add `internal_include` themselves |
| `mazda_telemetry` | `components/mazda_telemetry/include/mazda/vehicle_telemetry.hpp` | Mazda-specific façade, decoder adapter, publication, and subscriptions | Host target exports only its component include directory and the public Mazda contract target; receive/lifecycle/transport remain core-owned |
| `vehicle_telemetry` / `can_bus` | Companion core `components/vehicle_telemetry` / `components/can_bus` | Generic runtime, receive-only CAN lifecycle, queue API, and diagnostics | Imported at one pinned commit; the controller has no duplicate generic implementation or transmit operation |
| `vehicle_can_rx` | `vehicle_can_rx/vehicle_can_rx.h` | Explicit vehicle listen-only application binding | Driver dependencies remain private to the ESP-IDF component; no active-mode binding is present |
| `local_argb` | `local_argb/local_argb.h` | LED worker/policy composition and renderer-private state | The ordinary target carries no Mazda compatibility dependency or sink include root |
| `local_argb_sink_contract` / `local_argb_compat` | `local_argb/lighting_sink.hpp` / `local_argb/legacy_compat.hpp` | Explicit private sink handoff / retained migration adapter | The WeAct application selects the generic sink contract; the compatibility target is no longer part of the vehicle build and remains only as a migration/test seam |

The availability evaluator intentionally remains a lower-level Mazda API:
`mazda/availability.hpp` imports decoder-health and signal primitives for its
overloads, but it is not included by `mazda/facade_contracts.hpp`. Ordinary
facade consumers therefore see only value-copy contracts.

## Current include graph

The supported application-facing closure is:

```text
mazda/vehicle_telemetry.hpp
        -> mazda/facade_contracts.hpp
             -> mazda/freshness.hpp, mazda/notification.hpp,
                mazda/reading.hpp, mazda/types.hpp,
                vehicle_core/health.hpp, vehicle_core/time.hpp
        -> (through the target) public mazda + vehicle_core contracts

mazda/telemetry_contracts.hpp -> mazda/facade_contracts.hpp

mazda/availability.hpp -> mazda/notification.hpp
                       + vehicle_core/decoder_contracts.hpp,
                         health.hpp, signal.hpp, time.hpp

mazda/internal_contracts.hpp [explicit internal_include only]
        -> mazda/state.hpp + vehicle_core/decoder_contracts.hpp,
           vehicle_core/frame.hpp, vehicle_core/telemetry_contracts.hpp
```

The public closure must not acquire `vehicle_core/frame.hpp`,
`vehicle_core/decoder_contracts.hpp`, `vehicle_core/signal.hpp`,
`vehicle_core/notification_channel.hpp`, `mazda/decoder.hpp`,
`mazda/definitions.hpp`, `mazda/state.hpp`, `can_bus`, lighting, board,
driver, or RTOS headers transitively. Lower-level callers may request those
APIs explicitly through their own declared targets.

## Include/source compatibility

`vehicle_core/vehicle_core.hpp` remains the portable core umbrella supplied by
the pinned companion checkout and still
provides frame, signal, and time primitives. Mazda model types are no longer
owned by `vehicle_core`; callers should use explicit Mazda includes:

```cpp
#include "vehicle_core/frame.hpp" // vehicle_core::RawCanFrame
#include "mazda/state.hpp"        // mazda::VehicleState and VehicleStateStore
#include "mazda/decoder.hpp"      // mazda::candidate decoder API
```

`mazda/telemetry_contracts.hpp` remains the Stage 0 compatibility path, but it
now forwards only public façade contracts. It no longer re-exports decoder,
service, or lighting handoffs. Implementations and authorized internal tests
that need those values include `mazda/internal_contracts.hpp` with an explicit
`lib/mazda/internal_include` path. An ordinary consumer including that path
through only the public target must fail; this is an intentional source/build
boundary migration, not a runtime behavior change.

The existing availability evaluator names, overloads, outcomes, and include
path remain available to lower-level consumers. No public lifecycle, polling,
notification, or diagnostics signature is changed by the boundary handoff.

## Required boundary gate

The S2-C architecture contract checker is registered exactly once in
`tests/host/CMakeLists.txt` as `architecture_contracts`. It builds
`vehicle_core` in an isolated consumer project without Mazda or RTOS inputs,
builds and runs the project-owned vehicle binding adapter tests, runs
the receive-only/vehicle-boundary/lighting validators once, and confirms that
retired capture code has no active dependency. It does not claim runtime
linkage, ESP-IDF support, Arduino packaging, or physical vehicle
acceptance.

The checker supplied by S1.5-C remains registered exactly once as the
`public_header_boundary` CTest. It compiles the six public entry points
independently, inspects compiler dependency files, builds an isolated consumer
against `vehicle_telemetry_contracts`, and checks both ordinary-denied and
explicitly-authorized internal access. Its separate
`public_header_checker_regression` test preserves the negative fixtures.

Run the integrated host gate with:

```sh
cmake -S . -B build/host -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build build/host -j2
ctest --test-dir build/host -R public_header_boundary --output-on-failure
```

The full host CTest run remains the required regression command. Existing
listen-only, artifact-separation, and LED semantic safety checks are owned by
the single `architecture_contracts` registration, while the public-header
checker has a single registration and is not invoked separately by CI.

## Stage 3-A application composition

The final WeAct application composition is documented in
[`mcan-64-firmware-integration.md`](mcan-64-firmware-integration.md). Its
ordinary application code includes only the public `VehicleTelemetry` facade,
registers typed notices once, polls speed/RPM at its own cadence, and never
receives frames or drives the LED. Board safe defaults and the explicit local
ARGB startup-black frame complete before `VehicleTelemetry::start()` starts
vehicle CAN. The service's private semantic update is translated through the
Mazda-owned lighting policy to the generic `local_argb_sink_contract` sink.

The repository intentionally contains no active-mode or acknowledgement-capable
CAN binding. Such experiments must live outside the vehicle firmware and run
only on an isolated, protected test setup.
