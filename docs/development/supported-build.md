# Supported host builds

Portable host code uses C++17. CI runs the host tests with CMake `4.3.5` and
Ninja. CMake 3.20 remains the declared minimum, but CI does not exercise it. The host test dependency is doctest `v2.5.0`, pinned to commit
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
FetchContent cache. The generic companion core is pinned at release `0.2.1`;
for an offline build, point `VEHICLE_CAN_CORE_SOURCE_DIR` at an exact `0.2.1`
checkout. If a compiler, Ninja, network, or the CI CMake version is unavailable,
record the exact command and report that check as unavailable.

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

The repository `.clang-format` file selects C++17 with `Standard: c++17`. Use
any clang-format version that supports C++17 for local formatting. CI currently
installs Ubuntu's `clang-format` package; other versions can produce different
formatting output, which the CI check will catch.

## Host test inputs

Portable decoder and freshness tests use `tests/support/fake_clock.hpp` for an
injectable monotonic clock and `tests/support/direct_frame_feeder.hpp` to
deliver synthetic frames. These tests do not depend on the retired capture
parser. The repository uses reviewed synthetic fixtures; no parser or replay
API for the retired custom capture format remains in test infrastructure.
Follow the [vehicle-data policy](license-and-vehicle-data.md) when adding or
changing fixtures.

## Offline vehicle-profile artifact sets

`tools/generate_vehicle_profile.py` publishes compiler JSON, C++ catalog metadata,
the capability report, and an input/output hash inventory as one matching set.
This is an offline normalized-profile tool; importer, runtime and firmware profile
selection remain separate modules. It runs with Python alone, without building
host tests or executing a firmware binary.

Supply a strict v1 profile, a reviewed policy registry, every source as
`--source ID=PATH`, and additional recognition/generator data as
`--generator-input NAME=PATH`. Each source's bytes must match its manifest SHA-256.
All generator Python modules and the explicit revision are also hashed. Inputs
are identified by logical names; output artifacts contain no machine paths or
generation timestamps. Manifest and policy bytes are snapshotted once per run.
Their inventory entry uses A5's normalized combined identity, so JSON whitespace
and unordered-record spelling do not introduce a second raw-file identity.

For a software-only empty-profile example, create a policy registry in a build
directory:

```sh
mkdir -p /tmp/vehicle-profile-build
cat > /tmp/vehicle-profile-build/policies.json <<'JSON'
{"version":1,"profile_revisions":[["synthetic.desert","desert.v3","desert-recognition.v2"]],"handlers":[],"evidence":[],"units":[],"unit_conversions":[],"invalid_value_policies":[],"freshness_policies":[]}
JSON
python3 tools/generate_vehicle_profile.py \
  --manifest tests/fixtures/vehicle-profiles/synthetic-empty.json \
  --policies /tmp/vehicle-profile-build/policies.json \
  --generator-revision local.v1 --output-dir /tmp/vehicle-profile-build/artifacts \
  --print-directory
```

Repeat the command with `--check` for the authoritative non-mutating drift check.
Missing, stale or corrupt outputs return status 1 without creating directories,
rewriting files, or changing mtimes. The populated structural fixtures contain
placeholder source hashes; they require actual independently authored synthetic
source bytes and matching hashes before this executable tool accepts them.

The policy JSON object has exactly the eight keys in the example. Arrays encode
the existing `ValidationPolicies` registry: `profile_revisions` rows are
`[profile, semantic_revision, recognition_revision]`; `handlers` rows are
`[name, revision, [profile_ids], value_type, unit, minimum_inputs, maximum_inputs]`;
`evidence` rows are `[ref, confidence, eligibility]`; `unit_conversions` rows are
`[source_unit, target_unit, scale, offset]`; `freshness_policies` rows are
`[profile, canonical_key, timeout_ms, basis, evidence_ref]`. Units and invalid-value
policies are unique string arrays. Duplicate keys/records, unknown keys, wrong
types and invalid handler bounds fail before publication. These are reviewed
data references, with no executable policy expressions or implicit Mazda defaults.

The output root contains `sets/<digest>/` immutable generations and an atomic
`current.json` index. Resolve that index once and use its generation directory for
all reads. Reading each file through a freshly resolved index could cross a
concurrent publication boundary. CMake exposes that resolved immutable path.
Generation writes and verifies every file in a staging directory, then publishes
the set and replaces the index. A late-output exception leaves the previous index
and set intact. Concurrent writers can select either complete generation; the
last successful index replacement wins. Existing sets are retained for readers;
garbage collection and power-loss durability guarantees are outside this tool.

Opt-in host or ESP-IDF configure callers can use the same Python-only module:

```cmake
include("${CONTROLLER_ROOT}/tools/vehicle_profile.cmake")
vehicle_profile_generate(
  MANIFEST "${PROFILE_MANIFEST}" POLICIES "${PROFILE_POLICIES}"
  GENERATOR_REVISION reviewed.v1
  SOURCE_FILES "body=${BODY_DBC}" "powertrain=${POWERTRAIN_DBC}"
  GENERATOR_INPUTS "recognition=${RECOGNITION_PACK}"
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/vehicle-profile")
# Use VEHICLE_PROFILE_COMPILER_CATALOG, VEHICLE_PROFILE_INCLUDE_DIR and
# VEHICLE_PROFILE_CAPABILITY_REPORT from this single generation.
add_dependencies(my_catalog_consumer vehicle_profile_artifacts)
```

The function is called once per project and restricts outputs to the build tree.
Manifest, policy, source, explicit generator inputs and Python generator modules
are configure/build dependencies. A changed input causes CMake to reconfigure and
resolve the new generation. `vehicle_profile_artifacts` regenerates the selected
set when required. Missing or modified files inside an existing immutable set
fail closed with an `existing generation is corrupt` diagnostic; the tool never
overwrites that directory while readers may use it. Investigate the corruption,
then configure a fresh build directory/output root to recover and redirect all
consumers together. The focused CMake regression tests this failure and recovery.
Use the direct CLI `--check` for non-mutating checks: CMake's automatic reconfigure
is intentionally a generation step. No production composition root invokes this
module yet, and the generated catalogs alone provide no decoding or runtime support.
