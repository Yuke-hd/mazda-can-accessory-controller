---
title: Evaluating can-opener-js (the-can-opener) for this project
author: Ke
date: 2026-10-18
branch: main
cost: { minutes: 30, tokens: 142017 }
touches:
  - AGENTS.md
  - docs/README.md
  - docs/decisions/README.md
  - docs/specs/companion/ble-protocol.md
  - docs/specs/configuration/controller-config.md
  - docs/architecture/hardware/obd2-esp32s3-dongle-rev0.md
  - docs/decisions/0006-generated-output-catalog.md
  - docs/intent/dbc-vehicle-support.md
  - docs/specs/companion/output-catalog.md
  - docs/specs/dbc-vehicle-support.md
---

## Decided
- can-opener-js is a TypeScript app-side "virtual vehicle" library (DBC + YAML profiles, reactive state, query/action via a user-supplied BLE transport) built for its authors' own OBD dongle firmware. It is reference/prior art only, not a dependency.
- Possible reference value: its own DBC parser/profile model for docs/specs/dbc-vehicle-support.md, and its BLE spec (docs/vehicle-profile-ble-spec.md in their repo) as a comparison for the proposed OBD2 ESP32-S3 dongle.

## Tried and ruled out
- Using it in firmware: it is TS, while our decoding is C++17 in lib/.
- Using it as the companion-app layer: its transport expects raw CAN frames and request/response characteristics, but our companion BLE protocol publishes normalized signals.
- Its query()/action() paths transmit to the vehicle, which conflicts with the receive-only invariant.

## Watch out
- As of Oct 2026 it is v0.1.0 with 3 stars and no releases. package.json says MIT but the repo has no LICENSE file; confirm the license before borrowing any code (THIRD_PARTY_NOTICES.md, provenance rules).
- Repo-wide grep from the root timed out (>120s); scope searches to docs/ lib/ components/ or use trail grep.
