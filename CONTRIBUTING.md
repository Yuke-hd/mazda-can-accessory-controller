# Contributing

This project is an accessory controller for a safety-related vehicle network.
Every change must preserve strict listen-only CAN receive, bounded telemetry,
and fail-off lighting behavior. Host tests are evidence of software contracts;
they are not hardware or vehicle validation.

All repository artifacts are written in English. Direct conversation may use
other languages, but source, comments, documentation, tests, configuration,
commit messages, Issues, and pull requests must remain in English unless a
future localization requirement says otherwise.

## License and vehicle-data submissions

Project-authored source, documentation, tests, and tooling are licensed under
Apache-2.0 (`LICENSE`). Third-party material keeps its original license and
must be recorded in `THIRD_PARTY_NOTICES.md`. The opendbc-derived candidate
signal material remains MIT-licensed and requires exact provenance before any
new definition is copied or generated.

Never commit or attach a raw vehicle capture, VIN, credential, precise
location, absolute timestamp, or non-anonymized trip data. Real captures are
for private analysis unless transformed into a reviewed, anonymized fixture.
Follow [`docs/policies/license-and-vehicle-data.md`](docs/policies/license-and-vehicle-data.md)
before submitting any fixture or third-party-derived artifact.

For every submitted reviewed fixture or derived artifact, include this
authorization statement in the PR, or write `Not applicable` when none is
included:

> I confirm that I own or have permission to submit this artifact, that I have
> followed the license and attribution requirements, and that I have removed
> credentials, VIN/vehicle identifiers, precise location, absolute time, and
> non-anonymized trip data. I understand that this public repository may
> redistribute an accepted fixture under its recorded license and that the
> maintainers may reject or remove it if its provenance, privacy, or safety
> status cannot be verified.

This authorization does not permit vehicle-side CAN transmission, diagnostic
polling, or any action outside the receive-only safety boundary.

## Development workflow

1. Create a GitHub Issue for non-trivial work and record scope and acceptance
   criteria.
2. Create a short-lived branch from the latest `main`.
3. Keep each commit focused and run the relevant host tests and validators.
4. Open a PR using the repository template and link the native Issue.
5. Resolve review conversations and squash merge only after CI passes.

Use descriptive branch names such as `feat/<slug>`, `fix/<slug>`,
`docs/<slug>`, `test/<slug>`, or `chore/<slug>`.

## Local checks

```sh
cmake -S . -B /tmp/mazda-accessory-controller-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build /tmp/mazda-accessory-controller-host --parallel
ctest --test-dir /tmp/mazda-accessory-controller-host --output-on-failure
python3 tools/check_architecture.py --root . \
  --core-root /path/to/esp32-vehicle-can-core
```

`tools/check_architecture.py` owns the receive-only, WeAct, ARGB, and generic
dependency validators. It requires the pinned vehicle-core checkout configured
by `VEHICLE_CAN_CORE_SOURCE_DIR` when an offline source tree is used.

Format C/C++ with the pinned CI formatter when available. Use `apply_patch`
for focused edits and inspect `git diff` before submitting.

## Generic dependency updates

The controller consumes generic `vehicle_core`, `can_bus`, and
`vehicle_telemetry` from the companion source repository with ordinary CMake
`FetchContent` and ESP-IDF Component Manager Git dependencies. Keep the
repository URL and full immutable commit SHA synchronized across CMake and
manifests; after an ESP-IDF configure, review and commit the generated
`dependencies.lock` when Component Manager is available. The canonical source
repository is `https://github.com/Yuke-hd/esp32-vehicle-can-core`. Do not
reintroduce copied generic component directories into this controller
repository. Run a fresh
host configure, architecture validator, and test suite against the exact
candidate checkout before updating the pin.

## Safety and review checklist

- Confirm the vehicle image remains `firmware/weact-can485-v1.1` and uses
  `TWAI_MODE_LISTEN_ONLY` with a zero-length transmit queue.
- Keep CAN, decoder, and telemetry code independent from LED driver handles.
- Preserve black startup, stale/fault fail-off, watchdog bounds, and the
  GPIO4 onboard status / GPIO16 100-pixel strip mapping.
- Document any change to signal provenance, freshness, brightness, wiring,
  fuse/current budget, level shifting, or hardware validation scope.
- State which automated, hardware-bench, and vehicle tests were run or not
  run. A build is not evidence of physical vehicle behavior.

There is no active CAN transmit path in the accessory firmware. Do not add
one. Any future experimental transmit implementation must be isolated from
vehicle firmware, prominently marked, and reviewed as a separate safety
decision.

## Commit messages and pull requests

Use Conventional Commits:

```text
<type>(<scope>): <imperative summary>
```

Keep the summary lowercase, imperative, and within 72 characters where
practical. Use scopes such as `core`, `can`, `weact`, `mazda-kf`, `argb`,
`protocol`, `docs`, or `repo`. Explain safety and compatibility impact in the
body and use native GitHub references such as `Refs #123` or `Closes #123`.

Every PR body must include the related Issue or explicitly say none, summary
and rationale, automated/bench/vehicle tests run and not run, listen-only and
fail-off impact, hardware/pin/bitrate or signal provenance changes, risks and
deferred work, and whether it contains sensitive vehicle data, credentials, or
third-party material.
