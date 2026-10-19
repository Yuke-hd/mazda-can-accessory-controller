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
  `Yuke-hd/esp32-vehicle-can-core` and pinned at release `0.2.1`. Do not copy
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

<!-- trail:start -->
## Trail — repo context graph

This repo is indexed in `.trail/cache/`: small linked markdown nodes that explain each
system and carry exact file:line spans, kept in sync with the code through git.

For ANY task here — understanding how something works, finding where code lives,
or scoping a change — get context from the graph before grepping or opening
source files. Re-ask freely (it's cheap) and reuse literal identifiers you
already have (symbol, error string, file name) as the query. New to this repo?
Run `trail map` first — a token-budgeted orientation (dir clusters, hubs,
hotspots), no LLM, no key.

- Run `trail ask "<your question>" --source` → ranked nodes with the relevant
  code spans inlined (each hit's ≤8-line crux by default; `--full` for whole
  definitions when the crux isn't enough). Match the tool to the task shape:
  for understanding or editing, the top node IS the answer — cite its
  `covers:` file:line spans and edit straight from `--source`. For
  exhaustive tasks ("every occurrence / every caller of this pattern"), ranked
  results are top-N, not complete — run `trail grep "<literal>"` instead
  (exhaustive over indexed files, grouped by enclosing symbol), falling back
  to raw `grep -rn` only for unindexed files.
- `trail skeleton <file>` → every definition's signature + span, ~10× cheaper
  than reading the file; use it to skim an API surface.
- `trail callers <symbol>` gives precomputed, exact edges — who calls this.
  Add `--direction out` for what it calls, or `--depth N` to walk
  transitively for the full blast radius. For structural questions, skip
  ranking and use this directly.
- Or browse: `.trail/cache/INDEX.md` lists every node; follow the links.
- Monorepos and folders of multiple repos rank fairly across sub-projects —
  hits carry `[scope/]` labels naming which one they're from. Narrow with
  `trail ask "<task>" --in <scope>/` once you know where you're working.

If a returned span is truncated ("+N more lines"), open the file at that exact
range before finalizing. Only open source files when a node genuinely lacks a
needed detail, and then at the exact file:line the node points to — never
re-read whole files.

After big code changes, refresh the graph with `trail build` (deterministic,
no API key, $0).
<!-- trail:end -->
