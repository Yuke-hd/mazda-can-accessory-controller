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
limited to NimBLE, logging, the RTOS, `esp_timer`, `esp_system.h` (for the
controlled restart), the `companion_protocol`
codec and its own headers, it names no CAN, Mazda, telemetry, action-engine,
controller-config, LED-driver, GPIO or NVS symbols, and its ESP-IDF
requirements are limited to `bt`, `companion_protocol`, `esp_timer`,
`freertos` and `log`.

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
