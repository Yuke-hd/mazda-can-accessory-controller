# Telemetry contracts and test seams

Stage 0-A freezes the value and lifecycle interfaces used by later background
service work. The declarations are compile-only seams: they do not start
tasks, touch CAN, or provide firmware behavior. Stage 1.5 verifies that these
contracts remain usable without exposing their implementation handoffs.

## Contract locations

- The pinned companion core's `components/vehicle_core/include/vehicle_core/telemetry_contracts.hpp` contains the
  generic value-copy `Reading`, `Notification`, callback, availability, and
  validation contracts.
- The pinned companion core's `components/vehicle_core/include/vehicle_core/decoder_contracts.hpp` separates
  decoder validity from transport, message, and signal health. It is a
  lower-level header and is not part of an ordinary façade include closure.
- `lib/mazda/include/mazda/reading.hpp` and `notification.hpp` provide the
  Mazda-qualified aliases and callback names without importing evaluator or
  implementation headers.
- `lib/mazda/include/mazda/facade_contracts.hpp` contains Mazda-facing
  configuration, typed results, subscription generations, diagnostics, and
  public type aliases. `mazda/telemetry_contracts.hpp` is a compatibility
  umbrella that forwards this façade surface only.
- `lib/mazda/include/mazda/availability.hpp` retains the lower-level evaluator
  overloads and may import signal/decoder-health primitives when explicitly
  requested by a decoder/state consumer.
- `lib/mazda/internal_include/mazda/internal_contracts.hpp` contains raw
  decoder/service handoffs. It is available only through an explicit internal
  include path and is never re-exported from the public compatibility header.
- `components/mazda_telemetry/include/mazda/vehicle_telemetry.hpp` declares
  the non-copyable façade and its fixed polling/notification channels.
- `components/local_argb_sink_contract/include/local_argb/lighting_sink.hpp`
  is the explicit implementation-only value-only lighting sink target;
  generic RGB/deadline data crosses it, not Mazda enums, decoder health, or
  driver handles. `components/local_argb/private_include` remains reserved for
  renderer.hpp and is not exported to sink consumers.
- `components/local_argb_compat/include/local_argb/legacy_compat.hpp` is a
  temporary firmware-only adapter. The ordinary `local_argb` target does not
  compile or link its Mazda-dependent implementation.

The façade has two fixed subscriber slots per notification channel. Handles
carry channel, slot, and generation internally, so a stale handle cannot
remove a later registration. Configuration and subscription mutation are
stopped-only operations. The host thread or ESP-IDF task that performs the
first lifecycle mutation (configure, subscription, sink binding, start, or
stop) is its lifecycle owner and must perform later lifecycle mutations,
including stop/start cycles. Polling and diagnostics are
context-independent. The implementation records that owner with a bounded,
allocation-free 64-bit generation token rather than a thread-local address or
FreeRTOS task handle, so sequential host threads and recycled ESP-IDF task
handles cannot inherit an earlier owner's identity.

Callbacks receive copied values and borrow their caller-provided context until
`stop()` returns successfully. Callback-originated `configure`, `start`,
`stop`, `subscribe`, and `unsubscribe` calls are rejected before lifecycle
state or source side effects. A successful stop establishes callback and
worker quiescence; a timeout or other failure leaves the facade and callback
context live so the lifecycle owner can retry. Do not destroy the facade or
release callback context until that retry succeeds. Runtime task and driver
ownership is private to the background service. The Stage 3-A WeAct
application uses the public facade as shown in
[`mcan-64-firmware-integration.md`](mcan-64-firmware-integration.md); it does
not add a manual CAN, decode, freshness, notification, or LED loop.

## Deterministic host seams

`tests/support/fake_clock.hpp` provides an injectable monotonic clock and
`tests/support/direct_frame_feeder.hpp` delivers synthetic frames directly to
decoder tests. Decoder/freshness tests use these helpers and do not depend on
the retired capture parser. Capture-format tests were removed with the custom
capture product in S1-D; no parser or replay API remains in test
infrastructure. S2-C adds the standalone core-only and vehicle adapter
build probes through `architecture_contracts`, without requiring S2-A or S2-B
runtime sources.

## Boundary and compatibility checks

The S1.5-C checker is registered once in the host CTest flow as
`public_header_boundary`. It independently compiles the application-facing
headers, reads compiler dependency output for forbidden transitive headers,
checks the exported `vehicle_telemetry_contracts` consumer interface, and
probes both ordinary denial and explicit authorization for
`mazda/internal_contracts.hpp`.

The direct checker command is useful when iterating on a header boundary:

```sh
python3 tools/check_public_headers.py --root .
```

The fixture mutation tests remain separate checker-unit evidence and never
modify production files:

```sh
python3 tests/header_boundary/check_public_headers_test.py
```

The integrated gate is intentionally distinct from runtime linkage and
firmware acceptance. Passing it does not establish service calls, ESP-IDF or
Arduino support, CAN safety on hardware, or physical LED behavior.

## Fresh host validation

From a fresh build directory (CMake 3.20+ and a C++17 compiler):

```sh
cmake -S . -B build/host -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build build/host -j2
ctest --test-dir build/host --output-on-failure
```

The host run includes the consolidated `architecture_contracts` gate, the
required public-header boundary gate and regression fixtures, and the existing
portable contract tests. The architecture gate owns the receive-only,
vehicle artifact and local-ARGB semantic safety validators exactly once;
CI does not invoke them a second time before firmware builds. Physical
firmware and isolated hardware checks remain outstanding when their pinned
toolchain or hardware is unavailable; host contract tests never claim those
results. No credentials, private captures, or personal trip data are included
in these artifacts.

## Stage 1.5 validation evidence

On 2026-09-11, the fresh host run used CMake 4.4.3, AppleClang 17, C++17, and
the doctest commit pinned in `tests/host/CMakeLists.txt`:

```text
cmake -S . -B /tmp/mazda-accessory-controller-host -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-accessory-controller-host -j2
ctest --test-dir /tmp/mazda-accessory-controller-host --output-on-failure
```

Result: configure and build passed; all 78 CTest tests passed, including
`public_header_boundary`. The standalone checker passed all six public entry
points, the isolated consumer target, and the ordinary-denied/authorized
internal-access probes. The fixture mutation suite also passed all 10 tests:

```text
python3 tools/check_public_headers.py --root .
python3 tests/header_boundary/check_public_headers_test.py
```

The pinned ESP-IDF firmware checks remain outstanding on this host because
neither Docker nor `idf.py` is available. The repository's host toolchain
checker also reports `clang-format-14` unavailable; this does not affect the
compiled host result.

## S2-C architecture validation

The consolidated architecture command was run on 2026-09-12:

```text
python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake
```

It passed the isolated `vehicle_core` build/dependency check, the real
vehicle binding adapter build and execution, all three safety
validators, the active-path raw-capture removal check, and the single-validator
ownership check. A fresh host CTest run also passed the same gate and retained
the standalone public-header and consumer checks. AddressSanitizer,
UndefinedBehaviorSanitizer, and ThreadSanitizer targeted runs passed for the core
notification and CAN queue/lifecycle executables. ESP-IDF/Docker availability
remains a firmware-only prerequisite.
