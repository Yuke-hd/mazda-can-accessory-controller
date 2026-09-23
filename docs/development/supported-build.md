# Supported host builds

Stage 0-B keeps the host build reproducible across the supported CMake floor
and a modern CMake release. The supported minimum is CMake `3.20`; CI tests
that floor with `3.20.5` and the modern compatibility leg with `4.4.3`.
Both legs use Ninja, C++17, and the exact doctest `v2.5.0` commit pinned in
`tests/host/CMakeLists.txt`.

Run the checker before configuring:

```text
python3 tools/check_toolchain.py --scope host
```

Use a fresh temporary build directory for each verification. The following
commands are the baseline evidence recorded for this change (the build path
may be changed to another empty temporary directory):

```text
cmake -S . -B /tmp/mazda-accessory-controller-host -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/mazda-accessory-controller-host --parallel
ctest --test-dir /tmp/mazda-accessory-controller-host --output-on-failure
```

The doctest dependency is fetched by CMake at configure time. Network access
is therefore required on a clean build unless the pinned source is already in
the CMake FetchContent cache. No workaround policy flags are required. If a
compiler, Ninja, network, or CMake leg is unavailable, record that exact
command and mark the leg unavailable rather than claiming it passed.

## Sanitizer host build

CI also runs every portable host test and the Mazda telemetry service tests
under AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan). This
gate is intentionally reproducible on the same `ubuntu-22.04` Linux runner
with the Ubuntu `clang-14` package, CMake, and Ninja. The sanitizer runtime is
linked into each test executable and leak checking is enabled; a sanitizer
diagnostic prints a stack trace and fails the job.

The equivalent local commands are:

```text
sudo apt-get update
sudo apt-get install --no-install-recommends -y clang-14 cmake ninja-build python3
export CC=clang-14
export CXX=clang++-14
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1:print_summary=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
cmake -S . -B /tmp/mazda-accessory-controller-sanitizers -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build /tmp/mazda-accessory-controller-sanitizers --parallel
ctest --test-dir /tmp/mazda-accessory-controller-sanitizers --output-on-failure
```

This is a host-only sanitizer gate. The supported sanitizer combination is
limited to Linux with Clang 14 because LeakSanitizer behavior and runtime
availability differ on macOS, Windows/MSVC, and other compiler versions; the
ordinary host jobs remain the compatibility signal for those platforms. The
ESP-IDF firmware jobs are also excluded because their embedded toolchain does
not use the host sanitizer runtime. Do not disable leak detection or add a
blanket test exclusion when reproducing a failure: fix the reported test or
record the specific unsupported platform/toolchain instead.

The firmware builds remain separately pinned to ESP-IDF `v5.5.4`; see the
[MCAN-3 scaffold](mcan-3-scaffold.md) for the isolated vehicle
commands.
