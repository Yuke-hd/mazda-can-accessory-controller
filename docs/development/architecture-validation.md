# Architecture and boundary validation

The host validation tools check selected dependency and safety contracts.
They complement component tests; a passing host check does not establish
ESP-IDF behavior or physical vehicle acceptance. The portable host build and
full CTest workflow are in [Supported host builds](supported-build.md).

## Architecture contract gate

`architecture_contracts` is registered once in the host CTest flow. The
direct command is:

```sh
python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake
```

The checker builds an isolated consumer against the pinned companion core and
inspects both its compile/dependency output and the core target's own
translation units for Mazda, ESP-IDF, RTOS, CAN-driver, or other project
component inputs. It also builds and runs the project-owned vehicle receive
adapter tests against a test-only TWAI seam, checking listen-only mode, fixed
pins, and the disabled data-frame queue.

The portable `vehicle_signals`, `action_engine`, and `companion_protocol`
layers are each built and run in their own probe project against the core
alone. `companion_protocol` may link only `vehicle_signals`; its sources must
not name Mazda types, the BLE stack, CAN drivers, RTOS, or LED outputs outside
comments, and vehicle catalog key literals are allowed only in
`lib/companion_protocol/src/live_signal_layout.cpp`.

A source scan keeps `components/companion_ble` isolated: its includes are
limited to NimBLE, logging, the RTOS, `esp_timer`, the `companion_protocol`
codec, `vehicle_signals/signal_provider.hpp` and its own headers, it names no
CAN, Mazda, telemetry, action-engine, controller-config, LED-driver, GPIO or
NVS symbols, and its ESP-IDF requirements are limited to `bt`,
`companion_protocol`, `esp_timer`, `freertos`, `log` and `vehicle_signals`.
It cannot include `esp_system.h`: the controlled restart belongs to the
composition root, which fails lighting off first. The only `vehicle_signals`
name it may use is a `const vehicle_signals::SignalProvider`.
`subscribe`/`unsubscribe` calls are rejected, and so are `const_cast`,
`reinterpret_cast`, `remove_const`, `remove_cv`, `remove_cvref` and `decay`
anywhere in the component. This is a textual, defence-in-depth check for
honest mistakes, not a proof that no writable view exists; code review still
owns read-only provider access. Two reviewed casts are exempt, each only in its
own file: `const_cast<DeviceInfoValues *>` in
`device_info_characteristic.cpp`, which hands the Device info values to
NimBLE's `void *` access argument, and `reinterpret_cast<const std::uint8_t *>`
in `advertising.cpp`, which passes the advertised name as bytes. The GATT
table builder in `gatt_service.cpp` passes the registered, writable definition
slot as the access argument, so it needs no cast. Only `security.cpp` may read
NimBLE's `sec_state`; every other source that sends a notification or
indication must also call `link_has_accepted_bond()`, the notification gate in
[`ble-protocol.md`](../specs/companion/ble-protocol.md). Like the cast rule,
this is a textual check that a gate is present, not proof that every send is
gated.

A second source scan keeps `components/companion_config` isolated: its
includes are limited to its own headers, `companion_protocol`, controller-config
persistence, `action_engine`, `local_argb_actions`, the value-only
`local_argb/lighting_sink.hpp` contract, `vehicle_core/time.hpp` and the
`vehicle_signals` provider contract. It names no Mazda, CAN, telemetry, RTOS,
ESP-IDF, LED-driver, NVS or BLE symbols, never calls a sink or `attach()`, and
its CMake requirements are limited to `action_engine`, `companion_protocol`,
`controller_config`, `local_argb_actions`, `local_argb_sink_contract`,
`vehicle_core` and `vehicle_signals`. The lighting sink contract is allowed
only so the dry run can implement the discarding scratch sink.

The gate owns the receive-only, board-artifact, and local-ARGB semantic
validators, plus checks that retired raw-capture code has no active dependency.
Those source validators are not separately registered or repeated by firmware
CI. The historical protocol and work-item documentation remains outside the
active-path scan.

Negative fixtures for a forbidden core dependency and a retired marker in an
active build path can be run with:

```sh
python3 tests/tools/check_architecture_test.py -v
```

## Public header boundary gate

`public_header_boundary` compiles application-facing public headers in
isolated translation units, inspects compiler dependency output for forbidden
transitive headers, checks the exported `mazda_telemetry_contracts`
consumer interface, and probes denied ordinary access and explicitly
authorized access to `mazda/internal_contracts.hpp`. The detailed ownership
map and supported include boundaries are maintained in
[Module boundaries](../architecture/module-boundaries.md).

Run the checker directly while changing exported headers:

```sh
python3 tools/check_public_headers.py --root .
```

The fixture regression suite verifies forbidden transitive includes, exported
private include paths, and target-controlled conditional includes in a
temporary copy of the clean fixture. It never changes production files:

```sh
python3 tests/header_boundary/check_public_headers_test.py --compiler c++ --cmake cmake
```

The header checker and its fixture regression are registered once as
`public_header_boundary` and `public_header_checker_regression`. The stricter
isolated `vehicle_signals`, `action_engine`, and `companion_protocol`
dependency checks belong to
`architecture_contracts`.

The public-header gate verifies compilation and dependency boundaries. It does
not establish runtime linkage, ESP-IDF or Arduino support, CAN safety on
hardware, or physical LED behavior.
