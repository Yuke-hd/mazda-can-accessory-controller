# Supported host builds

Portable host code uses C++17. CI covers CMake `3.20.5` and `4.4.3` with
Ninja. The host test dependency is doctest `v2.5.0`, pinned to commit
`d44d4f6e66232d716af82f00a063759e9d0e50d6` in
`tests/host/CMakeLists.txt`. The exact pin avoids the pre-3.5 CMake minimum
in doctest `v2.4.11` metadata, which CMake 4 rejects. The firmware toolchain
is documented separately in [Firmware builds](firmware-build.md).

Check the host tools before configuring:

```sh
python3 tools/check_toolchain.py --scope host
```

Install the host-only Python dependencies used by the configuration compiler
and its CTest loader checks:

```sh
python3 -m pip install --user -r tools/requirements.txt
```

Use a fresh build directory for each verification:

```sh
cmake -S . -B /tmp/mazda-accessory-controller-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build /tmp/mazda-accessory-controller-host --parallel
ctest --test-dir /tmp/mazda-accessory-controller-host --output-on-failure
```

The doctest dependency is fetched by CMake at configure time. A clean build
therefore needs network access unless the pinned source is already in the
FetchContent cache. The generic companion core is pinned at release `0.1.0`;
for an offline build, point `VEHICLE_CAN_CORE_SOURCE_DIR` at an exact `0.1.0`
checkout. If a compiler, Ninja, network, or supported CMake leg is unavailable,
record the exact command and report that leg as unavailable.

## Sanitizers

CI also runs portable host tests and Mazda telemetry service tests under
AddressSanitizer and UndefinedBehaviorSanitizer on Linux with Clang 14. The
sanitizer runtime is linked into each test executable and leak checking is
enabled. Reproduce the gate with:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends -y clang-14 cmake ninja-build python3
export CC=clang-14
export CXX=clang++-14
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:print_summary=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
cmake -S . -B /tmp/mazda-accessory-controller-sanitizers -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build /tmp/mazda-accessory-controller-sanitizers --parallel
ctest --test-dir /tmp/mazda-accessory-controller-sanitizers --output-on-failure
```

This sanitizer gate is limited to Linux with Clang 14 because LeakSanitizer
runtime availability and behavior differ on macOS, Windows/MSVC, and other
compiler versions. Ordinary host jobs provide the compatibility signal for
those platforms. ESP-IDF firmware jobs do not use the host sanitizer runtime.
Do not disable leak detection or add a blanket test exclusion when reproducing
a failure; fix the reported test or record the specific unsupported
platform/toolchain.

The formatter checked by CI is clang-format major version `14`.

## Host test inputs

Portable decoder and freshness tests use `tests/support/fake_clock.hpp` for an
injectable monotonic clock and `tests/support/direct_frame_feeder.hpp` to
deliver synthetic frames. These tests do not depend on the retired capture
parser. The repository uses reviewed synthetic fixtures; no parser or replay
API for the retired custom capture format remains in test infrastructure.
Follow the [vehicle-data policy](license-and-vehicle-data.md) when adding or
changing fixtures.
