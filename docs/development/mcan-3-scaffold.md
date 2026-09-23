# MCAN-3 scaffold

This change establishes build boundaries for the first firmware milestone and
portable host library. It does not decode Mazda signals, record captures, export
telemetry, or provide a CAN data-generation path.

## Pinned toolchains

- ESP32 firmware: ESP-IDF `v5.5.4`, target `esp32`.
- Host library and tests: C++17, CMake `3.20` minimum (CMake `4.4` modern
  compatibility leg), clang-format major version `14`, doctest tag `v2.5.0` at
  commit `d44d4f6e66232d716af82f00a063759e9d0e50d6` (MIT license).

The ESP-IDF manifest requires exactly `5.5.4`; it does not download an
unrelated SDK at configure time. The host test dependency is fetched by CMake
using the exact doctest commit above. v2.5.0 is required because the previous
v2.4.11 CMake metadata used a pre-3.5 minimum rejected by CMake 4; no unpinned
dependency is used.

Before running a build, check the active executable versions. The checker is
non-interactive and reports all failures in the selected scope:

```text
python3 tools/check_toolchain.py --scope host
python3 tools/check_toolchain.py --scope firmware
```

The checker does not install tools or change system packages. Install and
activate ESP-IDF `v5.5.4` with the official Espressif guide before the firmware
check.

## Reproducible commands

From the repository root:

```text
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

For the ESP-IDF application, install and activate ESP-IDF `v5.5.4`, verify the
firmware scope, and run:

```text
cd firmware/weact-can485-v1.1
python3 ../../tools/check_toolchain.py --scope firmware
idf.py set-target esp32
idf.py build
```

## Safety and scope

The MCAN-3 baseline was deliberately a structure-only scaffold. The board
component now contains the exact WeAct CAN485 DevBoard V1.1 capability record
and applies fail-closed electrical defaults: CAN TX recessive, RS485 disabled,
and the GPIO4/GPIO16 WS2812 data lines low with no new command pulses. MCAN-16
adds the required RMT black frame before CAN startup. MCAN-7 extends that
baseline with a bounded strict listen-only receive path; it still provides no
transmit API.

CI runs the host checks and WeAct vehicle build
on every pull request.
These are compile/configuration checks, not bench or vehicle validation.


Dependency source, exact version/commit, role, and upstream license links are recorded in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md). The upstream notices remain authoritative.
