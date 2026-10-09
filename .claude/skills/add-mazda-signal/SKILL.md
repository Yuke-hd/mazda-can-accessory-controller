---
name: add-mazda-signal
description: Add a new vehicle signal to the Mazda CAN accessory controller, from CAN field evidence through decoder, provider catalog, config compiler manifest, and optional replay or companion exposure. Use when asked to implement, decode, or expose a new signal.
---

# Add a Mazda signal

The full procedure is in `docs/development/adding-a-signal.md`. Read it first;
this skill is the short driver.

## Workflow

1. Read `docs/development/adding-a-signal.md`, `AGENTS.md`, and the acceleration
   example commits it lists: acceleration (`044939a`, `3b95e6b`, `0efbb92`, `cd79a0a`) for a Number signal and brake (`d3b7bba`) for a Boolean or Enum notify signal.
2. Pin down the signal: type (Boolean/Enum/Number), canonical key, unit, source
   message ID and bit layout, and where the field comes from. If the source or
   confidence is unclear, stop and ask. Never invent evidence.
3. Work in order, one reviewable change per step:
   evidence docs -> decoder in `lib/mazda/` -> provider catalog and descriptor in
   `components/mazda_telemetry/` -> `tools/controller_signal_catalog.json` ->
   optional replay/companion surfaces.
4. Write tests with synthetic vectors only: neutral, boundary, wrong DLC,
   malformed-then-recovered, older and same-timestamp frames.
5. Validate with the host build, ctest, `tools/check_architecture.py`,
   `tools/check_public_headers.py`, and clang-format 14 (see
   `docs/development/build-modes.md`).

## Hard rules

- Receive-only: no transmit, diagnostics, polling, or ACK.
- Leave freshness timeouts and message periods unset without timing evidence.
- Append new `SignalId`s; never renumber. Keys are persisted and on the wire.
- Do not promote evidence confidence from synthetic tests.
- Never commit captures, VINs, or trip data.
- Report host results separately from hardware or vehicle validation.
