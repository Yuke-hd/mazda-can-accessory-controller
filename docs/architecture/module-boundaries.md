# Module and header boundaries

The module layout separates portable transport primitives from Mazda
semantics, keeps public contracts value-only, and isolates hardware bindings
at the firmware boundary.

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
| `local_argb` | `local_argb/local_argb.h` | LED worker/policy composition and renderer-private state | The ordinary target carries no Mazda compatibility dependency or sink include root; it re-exports only the frame contract root |
| `local_argb_pixel_frame` (host target; files live in `local_argb_sink_contract/frame_include/`) | `local_argb/pixel_frame.hpp` | `Rgb`, `kBlack`, `kLedCount`, `PixelFrame`, `kBlackFrame`, and `PixelFrameSink` | Header depends on no renderer, ESP-IDF, `vehicle_core`, or Mazda code; frame consumers such as `replay_pixel_output` link `local_argb_pixel_frame`, not `local_argb` or `local_argb_sink_contract` (which also exports the sink root and `vehicle_core`) |
| `local_argb_sink_contract` / `local_argb_compat` | `local_argb/lighting_sink.hpp` / `local_argb/legacy_compat.hpp` | Explicit private sink handoff / retained migration adapter | The WeAct application selects the generic sink contract; the compatibility target is no longer part of the vehicle build and remains only as a migration/test seam |
| `gvret_parser` (host-only) | `gvret/csv_parser.hpp`, `gvret/replay_stream.hpp`, `gvret/file_loader.hpp` | SavvyCAN/GVRET CSV parsing, bus selection, and timestamp normalization | Links only `vehicle_core`; no replay, controller, action, or renderer target |
| `replay` (host-only, `lib/replay`) | `replay/output_stage.hpp`, `controller.hpp`, `scheduler.hpp`, `local_argb_stage.hpp`, `pixel_frame_output.hpp` | Replay clock, source, controller, scheduler, output stages, and the `can-replay` CLI | The controller and scheduler drive an injected `replay::OutputStage` and include no `local_argb` private or internal types; only `replay_local_argb_stage` wires the local ARGB renderer |

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

`mazda/telemetry_contracts.hpp` is the compatibility include for public
façade contracts. It does not re-export decoder, service, or lighting handoffs.
Implementations and authorized internal tests that need those values include
`mazda/internal_contracts.hpp` with an explicit `lib/mazda/internal_include`
path. An ordinary consumer using only the public target must not gain access to
that internal path.

`mazda/availability.hpp` remains a lower-level API separate from the ordinary
facade. Public lifecycle, polling, notification, and diagnostics contracts
remain value-only.

## Validation and application composition

The host architecture and public-header checks enforce these contracts; the
commands and validator ownership are documented in
[`architecture-validation.md`](../development/architecture-validation.md).
The WeAct application composition is documented in
[`firmware-composition.md`](firmware-composition.md): the ordinary application
uses the public `VehicleTelemetry` facade, registers typed notices, polls
speed/RPM at its own cadence, and never receives frames or drives the LED.
Safe board defaults and the explicit local ARGB startup-black frame complete
before `VehicleTelemetry::start()` starts vehicle CAN. The current lighting
path is `MazdaSignalProvider` -> `ActionEngine` -> `LedActionSink` -> the
generic `local_argb_sink_contract` sink; the legacy Mazda lighting binding is
not selected by the firmware.

The repository intentionally contains no active-mode or acknowledgement-capable
CAN binding. Such experiments must live outside the vehicle firmware and run
only on an isolated, protected test setup.
