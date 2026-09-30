# Telemetry boundary validation history

This note preserves implementation-time validation evidence. It records
results from the dates below and does not establish the current state of the
toolchain or checkout. Current procedures are in
[Architecture and boundary validation](../development/architecture-validation.md)
and [Supported host builds](../development/supported-build.md).

## 2026-09-11: public header boundary

The fresh host run used CMake `4.4.3`, AppleClang 17, C++17, and the doctest
commit pinned in `tests/host/CMakeLists.txt`:

```sh
cmake -S . -B /tmp/mazda-accessory-controller-host -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-accessory-controller-host -j2
ctest --test-dir /tmp/mazda-accessory-controller-host --output-on-failure
```

Configure and build passed, and all 78 CTest tests passed, including
`public_header_boundary`. The standalone checker passed all six public entry
points, the isolated consumer target, and the ordinary-denied and
explicitly-authorized internal-access probes. Its fixture mutation suite
passed all 10 tests:

```sh
python3 tools/check_public_headers.py --root .
python3 tests/header_boundary/check_public_headers_test.py
```

At that time, the pinned ESP-IDF firmware checks were unavailable because
neither Docker nor `idf.py` was available. The host toolchain checker also
reported `clang-format-14` unavailable; neither condition affected the
compiled host result.

## 2026-09-12: architecture contracts

The consolidated architecture checker was run with:

```sh
python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake
```

It passed the isolated `vehicle_core` build and dependency check, the real
vehicle binding adapter build and execution, all three safety validators, the
active-path retired raw-capture check, and the single-validator ownership
check. A fresh host CTest run passed the same gate and retained the standalone
public-header and consumer checks.

The implementation-host notes also record that AppleClang 17 ran targeted
AddressSanitizer and UndefinedBehaviorSanitizer checks for core notification
and CAN queue/lifecycle executables, plus ThreadSanitizer checks for those
executables; they passed. ESP-IDF/Docker availability remained a firmware-only
prerequisite. These host checks did not establish physical vehicle or LED
acceptance.
