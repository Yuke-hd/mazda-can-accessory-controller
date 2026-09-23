# S2-C architecture contract checks

The S2-C host gate consolidates project-owned architecture validation without
making unfinished runtime work a prerequisite. The root host CMake registers
one `architecture_contracts` CTest, which runs the existing source-safety
validators once and adds compiled probes for the module boundaries.

## Compiled boundaries

The gate configures and builds a temporary consumer against only
the pinned companion core checkout. The consumer links `vehicle_core`, exercises its portable
frame validity function, and checks value-copy reading/notification types. Its
compile command and dependency output, together with the `vehicle_core`
target's own translation-unit entries and dependency outputs, are inspected for
Mazda, ESP-IDF, RTOS, CAN-driver, and other project component inputs. A
private dependency introduced by `vehicle_core` therefore fails the gate even
when the generated consumer remains clean. This is an actual target boundary
check; the root Mazda or firmware projects are not pulled into the core-only
build.

The gate also configures, builds, and runs the project-owned adapter tests in
`components/vehicle_can_rx/tests`. These compile the real binding translation
unit against the test-only TWAI seam and assert the vehicle `LISTEN_ONLY` mode,
fixed pins, and disabled data-frame queue. No runtime mode selector is
introduced.

## Validator ownership

`validate_can_receive_only.py`, `validate_weact_vehicle_artifacts.py`, and
`validate_local_argb_boundary.py` are invoked by `architecture_contracts` and
are not separately registered in host CTest or repeated in firmware CI. The
source checks remain because firmware integration and startup safety still
need their explicit guards; compiled adapter probes provide the complementary
behavioral evidence. The Stage 1.5 `public_header_boundary` and
`public_header_checker_regression` gates remain independent and registered
exactly once each.

The same gate scans active code, test, build, and workflow paths—including
every `CMakeLists.txt`, `.cmake`, `.yml`, and `.yaml` file under the active
component, firmware, library, test, and tool roots—for the retired
`raw_capture` product markers. Historical protocol/development notes are not
treated as active dependencies. No source DBC or private capture data is read
or published.

## Verification

Run the consolidated check directly from the repository root:

```sh
python3 tools/check_architecture.py --root . --compiler c++ --cmake cmake
```

The deterministic negative fixtures for a private `vehicle_core` dependency
and a retired marker in active build files can be run directly with:

```sh
python3 tests/tools/check_architecture_test.py -v
```

The normal host suite runs it through CTest:

```sh
cmake -S . -B build/host -G Ninja -DBUILD_TESTING=ON -DMAZDA_BUILD_HOST_TESTS=ON
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

The host CTest suite is also exercised by the supported CMake compatibility
legs. On the implementation host, AppleClang 17 supported AddressSanitizer,
UndefinedBehaviorSanitizer, and ThreadSanitizer: the core notification and
CAN queue/lifecycle executables passed under ASan/UBSan and TSan. Firmware
Docker/ESP-IDF remains unavailable on this host and is reported separately;
none of these host checks is physical acceptance.
