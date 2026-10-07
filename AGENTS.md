# Repository Instructions

## Scope and sources of truth

- This repository is an ESP32 Mazda CAN accessory-lighting controller. Read
  `README.md` for orientation and `CONTRIBUTING.md` for contributor procedures.
- Keep repository artifacts in English: source, comments, tests, docs,
  configuration, commits, Issues, and pull requests.
- Treat host tests and source validators as software evidence only. Never
  describe a successful build or test as hardware or vehicle validation.

## Documentation

Consult documentation relevant to the change. `docs/README.md` routes common
tasks to the current documents.

- `docs/architecture/` — system boundaries and architectural contracts.
- `docs/specs/` — current intended component and feature behaviour.
- `docs/protocol/` — Mazda CAN definitions, evidence, and provenance.
- `docs/development/` — build, test, and contributor procedures.
- `docs/work-items/` — planning/historical material; not authoritative unless
  explicitly referenced by the task.

For replay changes, consult the relevant `docs/specs/replay/` specs. For
component-boundary changes, consult `docs/architecture/module-boundaries.md`
and `docs/development/architecture-validation.md`. For signal changes, consult
`docs/protocol/signal-evidence.md` and the relevant telemetry specs. For
hardware changes, consult `docs/architecture/hardware/weact-can485-v1.1.md`.

## Safety invariants

- Vehicle firmware is receive-only classic CAN. Preserve
  `TWAI_MODE_LISTEN_ONLY`, a zero-length transmit queue, and the absence of
  application transmit, diagnostic polling, and vehicle ACK behavior.
- Unknown, stale, malformed, unavailable, stopped, expired, or faulted
  telemetry must fail off to black. Preserve startup black, immediate
  driver-failure fail-off, bounded retries/watchdogs, and recovery only after
  a newer valid observation where required.
- Brake freshness is intentionally unset pending reviewed timing evidence.
  Do not invent a timeout or promote brake observations to `Fresh`. Brake
  output is enabled only through the owner-approved `fresh_or_unverified`
  opt-in on `vehicle.brake_pressed` (the factory `brake` action); keep other
  brake paths requiring `Fresh`.
- Preserve the reviewed WeAct CAN485 V1.1 hardware capabilities; use the
  hardware record above for pin assignments and validation requirements.
- Keep CAN acquisition, Mazda decoding, telemetry/publication, lighting
  policy, and LED rendering separated. Telemetry producers must not perform
  LED-driver work.

## Architecture and dependencies

- Portable code is C++17. Hardware-independent decoder and policy logic
  belongs in `lib/`; ESP-IDF bindings belong in `components/` or the sole
  firmware target at `firmware/weact-can485-v1.1/`.
- Generic `vehicle_core`, `can_bus`, and `vehicle_telemetry` are owned by
  `Yuke-hd/esp32-vehicle-can-core` and pinned at release `0.2.0`. Do not copy
  those components into this repository. Keep the CMake pin and all ESP-IDF
  manifest references synchronized when updating that dependency.
- Preserve the public/internal include boundaries in the module ownership map.
- Do not restore retired raw-capture products or active build references.
  Synthetic, reviewed test fixtures are the supported test-data path.
- `graphify-out/`, `build/`, `managed_components/`, and the local companion
  checkout under `third_party/esp32-vehicle-can-core/` are generated or local
  state; do not hand-edit or commit them.

## Vehicle data and provenance

- Never commit or attach raw vehicle captures, VINs, credentials, precise
  locations, absolute trip timestamps, or reconstructable/non-anonymized trip
  data. Follow `docs/development/license-and-vehicle-data.md` for fixtures.
- Signal definitions, DBC-derived material, and timing/freshness claims need
  exact source, version, license, and confidence evidence. Update
  `THIRD_PARTY_NOTICES.md` when third-party attribution changes.

## Build and validation

- Use the fresh out-of-tree host build and relevant checks in
  `docs/development/supported-build.md`. Boundary changes also use
  `docs/development/architecture-validation.md`.
- Firmware work uses ESP-IDF 5.5.4 and target `esp32`; follow
  `docs/development/firmware-build.md` for SDK activation and build commands.
- Report automated, bench, and vehicle validation separately, including what
  was not run. Flashing or interacting with hardware requires explicit task
  scope and verification of the actual port, wiring, fuse, power, and board.
