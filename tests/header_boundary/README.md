# Public header boundary checker

`tools/check_public_headers.py` is the required Stage 1.5 host regression
checker for the application-facing C++ headers, including the frozen
`mazda/telemetry_contracts.hpp` compatibility entry point. It compiles each
entry point in a fresh translation unit, reads the compiler-generated
dependency file, and then builds a tiny CMake consumer against the real
`vehicle_telemetry_contracts` target. The consumer probe checks the exported
include flags as well as compilation, so an accidental `private_include`
export is reported even when the source happens to compile.

Run it directly from the repository root:

```sh
python3 tools/check_public_headers.py
```

The same checker is registered exactly once as the `public_header_boundary`
CTest in `tests/host/CMakeLists.txt`. A full host build therefore runs it with
the existing listen-only, artifact-separation, and LED semantic safety checks.
The checker is expected to pass after the S1.5 A/B header split.

The required `public_header_checker_regression` CTest proves the checker
itself. It copies the clean fixture to a temporary directory, adds a
forbidden transitive include, exported private path, or target-controlled
conditional include there, and verifies that the corresponding diagnostic is
returned. Production files are never mutated. CTest passes its selected
compiler and CMake executable to this suite:

```sh
python3 tests/header_boundary/check_public_headers_test.py --compiler c++ --cmake cmake
```

The checker builds separate facade-only and explicit lower-level consumers.
It inspects the facade dependency file produced by the exact CMake consumer
compile command, including target-controlled definitions. The checker also
runs two access probes. A normal consumer is expected to fail
when it includes `mazda/internal_contracts.hpp`; an explicitly authorized
consumer must be given the `lib/mazda/internal_include` directory and must
succeed. The legacy `private_include` directory is not an accepted internal
boundary; it remains a forbidden legacy/export marker. The public compatibility
location must remain inaccessible for ordinary consumers, while the authorized
probe remains useful evidence for explicitly granted implementation access.
