# Build modes

Every supported way to build, test, and check this repository, with the
command, the tools it needs, and where CI runs it. Details live in the linked
documents; this page is the index. Use a fresh out-of-tree build directory for
each mode so an old configuration cannot hide a change.

All modes are software evidence only. None establishes hardware, bench, or
vehicle validation.

## Summary

| Mode | Purpose | Tools | In CI |
| --- | --- | --- | --- |
| [Host build and tests](#1-host-build-and-tests) | Build portable libraries, run all host tests | CMake, Ninja, C++17 compiler, Python 3 | Yes |
| [Host with probes and profiling](#2-host-with-probes-and-profiling) | Same, with extra host-only instrumentation | As above | Yes |
| [Host with ASan + UBSan](#3-host-with-asan--ubsan) | Memory and undefined-behaviour checks | Clang 14 (Linux) | Yes |
| [Offline host build](#4-offline-host-build) | Build without fetching the core dependency | As host, plus a core checkout | No |
| [Firmware build](#5-firmware-build) | Compile the ESP32 application | ESP-IDF 5.5.4 | Yes |
| [Firmware diagnostics](#51-firmware-diagnostic-builds) | Opt-in runtime stats, stage profiling, freshness debug | ESP-IDF 5.5.4 | No |
| [Firmware with a custom profile](#6-firmware-with-a-custom-profile) | Embed a local controller config | ESP-IDF 5.5.4 | No |
| [Replay tools](#7-replay-tools) | Run `can-replay` and `gvret-led-emulator` on the host | Host build | Built with the host build |
| [Validators](#8-validators-and-formatting) | Architecture, boundary, and format checks | Python 3, clang-format 14 | Yes |
| [Telemetry baseline](#9-telemetry-baseline) | Synthetic performance measurement | Host build, Python 3 | No |

Check your tools first. This never installs anything:

```sh
python3 tools/check_toolchain.py --scope host       # host modes
python3 tools/check_toolchain.py --scope firmware   # after activating ESP-IDF
```

## 1. Host build and tests

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

- C++17. CMake 3.20 is the declared minimum; CI uses CMake 4.3.5.
- `MAZDA_BUILD_HOST_TESTS` defaults to the value of `BUILD_TESTING`.
- Configuring fetches doctest and the pinned core dependency, so a clean build
  needs network access (see mode 4 for offline).
- Install Python test dependencies once:
  `python3 -m pip install --user -r tools/requirements.txt`.

Details: [Supported host builds](supported-build.md).

## 2. Host with probes and profiling

CI configures the host build with two extra options:

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DMAZDA_ENABLE_TELEMETRY_PROFILING=ON \
  -DMAZDA_ENABLE_DECODER_CALL_PROBING=ON
```

| Option | Default | Effect |
| --- | --- | --- |
| `MAZDA_ENABLE_DECODER_CALL_PROBING` | `OFF` | Compiles host-only decoder routing call-count probes used by tests |
| `MAZDA_ENABLE_TELEMETRY_PROFILING` | `OFF` | Enables telemetry profiling support and its tests |

`MAZDA_ENABLE_TELEMETRY_STAGE_PROFILING=ON` additionally enables the host
stage-profiling tests ([Stage profiling](telemetry-stage-profiling.md)).

All are host-only instrumentation and are not part of firmware builds.

## 3. Host with ASan + UBSan

Linux with Clang 14 only. This is the CI gate.

```sh
export CC=clang-14 CXX=clang++-14
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:print_summary=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
cmake -S . -B build/sanitizers -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON \
  -DMAZDA_ENABLE_TELEMETRY_PROFILING=ON -DMAZDA_ENABLE_DECODER_CALL_PROBING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/sanitizers --parallel
ctest --test-dir build/sanitizers --output-on-failure
```

Leak detection stays on; do not add blanket exclusions. Other platforms and
compilers rely on the ordinary host job for compatibility. Details:
[Sanitizers](supported-build.md#sanitizers).

## 4. Offline host build

The generic CAN core (`esp32-vehicle-can-core`, release `0.2.1`) is normally
fetched by CMake. For offline work, point at an exact `0.2.1` checkout:

```sh
cmake -S . -B build/host -G Ninja -DBUILD_TESTING=ON \
  -DVEHICLE_CAN_CORE_SOURCE_DIR=/path/to/esp32-vehicle-can-core
```

| Cache variable | Default | Meaning |
| --- | --- | --- |
| `VEHICLE_CAN_CORE_REPOSITORY` | upstream GitHub URL | Where to fetch the core |
| `VEHICLE_CAN_CORE_TAG` | `0.2.1` | Release tag to fetch |
| `VEHICLE_CAN_CORE_SOURCE_DIR` | `third_party/esp32-vehicle-can-core` | Local checkout; used instead of fetching when it exists |

doctest is also fetched at configure time and is not covered by this override.
Do not copy core components into this repository. When changing the pin, keep
CMake and all ESP-IDF manifests in sync.

## 5. Firmware build

ESP-IDF `v5.5.4`, target `esp32`, sole target `firmware/weact-can485-v1.1`.

```sh
source /path/to/esp-idf/v5.5.4/export.sh
python3 tools/check_toolchain.py --scope firmware
cd firmware/weact-can485-v1.1
idf.py set-target esp32
idf.py build
```

CI builds this inside the `espressif/idf:v5.5.4` container. A compile is not
vehicle validation. Flashing needs explicit task scope and verification of the
actual port, wiring, fuse, power, and board; never run `idf.py erase-flash` on a
board with stored config unless intended. Partition layout and upgrade or
downgrade hazards are in [Firmware builds](firmware-build.md#partition-layout).

For a firmware build against a local core checkout, set
`VEHICLE_CAN_CORE_SOURCE_DIR` (cache path in the firmware `CMakeLists.txt`).

The build pins the hot telemetry functions to IRAM and fails if a pinned symbol
did not reach IRAM (`tools/check_iram_placement.py`), and it requires 80 MHz DIO
flash. See [Flash-cache layout](flash-cache-layout.md) before changing hot
functions or IRAM-consuming options.

### 5.1 Firmware diagnostic builds

All diagnostics are off in `sdkconfig.defaults`. Enable them through Kconfig or
an extra defaults file, in an out-of-tree build directory:

| Diagnostic | How to enable | Doc |
| --- | --- | --- |
| FreeRTOS task runtime stats | `SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.runtime-stats.defaults'` (or `...runtime-stats-synthetic.defaults` for the isolated synthetic profile) | [FreeRTOS runtime diagnostics](freertos-runtime-diagnostics.md) |
| Telemetry stage profiling | `CONFIG_WEACT_CAN_TELEMETRY_PROFILING=y` | [Stage profiling](telemetry-stage-profiling.md) |
| `0x091` freshness debug | `CONFIG_WEACT_CAN_FRESHNESS_DEBUG=y` | [CAN freshness debug](can-freshness-debug.md) |

## 6. Firmware with a custom profile

By default the firmware embeds `config/default.yaml`. To embed a local profile:

```sh
cd firmware/weact-can485-v1.1
idf.py -DCONTROLLER_CONFIG_DEFAULT_YAML=/path/to/profile.yaml build
```

- Keep local profiles in `config/`; everything there except `default.yaml` is
  git-ignored.
- The override is a CMake cache entry and persists in that build directory.
  Configure output prints the selected path and the SHA256 of the generated
  factory JSON; check both before using an image.
- Malformed YAML fails the firmware configure step. The ESP32 has no YAML
  parser; compilation to JSON is host-only.

To compile a profile to canonical JSON without building firmware:

```sh
python3 tools/compile_controller_config.py config/default.yaml /tmp/controller-config-v1.json
```

See the [controller config spec](../specs/configuration/controller-config.md).

## 7. Replay tools

`can-replay` and `gvret-led-emulator` are built by the host build (mode 1).
Find the executables under the build directory, e.g.:

```sh
find build/host -type f \( -name can-replay -o -name gvret-led-emulator \)
```

Specs: [pixel-frame output](../specs/replay/pixel-frame-output.md) and
[web emulator](../specs/replay/web-emulator.md). Use synthetic fixtures only;
output from a real capture is derived vehicle data and follows the
[vehicle-data policy](license-and-vehicle-data.md).

## 8. Validators and formatting

| Check | Command | Doc |
| --- | --- | --- |
| Architecture contracts | `python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake` | [Architecture validation](architecture-validation.md) |
| Public headers | `python3 tools/check_public_headers.py --root .` | [Architecture validation](architecture-validation.md) |
| IRAM placement | Runs automatically as a firmware POST_BUILD step (`tools/check_iram_placement.py`) | [Flash-cache layout](flash-cache-layout.md) |
| Toolchain checker tests | `python3 -m unittest discover -s tests/tools -p '*_test.py'` | CI |
| Formatting | `clang-format --dry-run --Werror <files>` | CI uses clang-format **14** |

Format check over the same files CI covers:

```sh
clang-format --dry-run --Werror $(find lib components firmware tests -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) -print)
```

`architecture_contracts` also runs inside the host CTest flow. Other
validators (`validate_can_receive_only.py`, `validate_local_argb_boundary.py`,
`validate_weact_vehicle_artifacts.py`) are described in the architecture
validation doc and the component specs.

## 9. Telemetry baseline

A synthetic host performance measurement, run on demand and not a CI gate.
Follow [Telemetry performance baseline](telemetry-performance-baseline.md).

## Reporting

Report automated host results, firmware compilation, bench results, and vehicle
results separately, and state what was not run. If a tool, network, or CI
version is unavailable, record the exact command and report that check as
unavailable.
