---
title: Adding a second action sink and firmware variants
author: Ke
date: 2026-10-18
branch: main
cost: { minutes: 2, tokens: 64018 }
touches:
  - AGENTS.md
  - docs/README.md
  - docs/architecture/hardware/obd2-esp32s3-dongle-rev0.md
  - docs/intent/dbc-vehicle-support.md
  - docs/specs/dbc-vehicle-support.md
  - skills-lock.json
  - tools/check_architecture.py
---

## Decided
- companion_ble is sink-agnostic: it depends only on companion_protocol + vehicle_signals (enforced by tools/check_architecture.py) and moves config JSON as opaque bytes via ConfigCommitter/ConfigBootSource ports. A new sink never needs a second BLE stack.
- LED coupling lives in (1) controller_config persisted schema: `outputs[]` OutputType is only led_effect/led_fill/led_transient/led_solid (model.hpp, json_loader.cpp), and (2) firmware main.cpp constructing LedActionSink directly.
- New sink path: new component implementing action_engine::ActionSink, register via action_engine::SinkFanOut (capacity 4) in main.cpp, add a new output type to the schema and bump kSchemaVersion. No companion_ble change.
- Firmware variants recommendation: one ESP-IDF project per board under firmware/<board>/ in this repo, each with its own main.cpp composition root; Kconfig only for debug/profiling toggles; optional sinks on the same board are enabled by runtime config, not compile flags. Separate repo only for non-Mazda products (consume esp32-vehicle-can-core).

## Tried and ruled out
- Kconfig model-selection matrix: chips (esp32 vs esp32s3), pins and per-board safety validation differ, so flag combinations become hard to audit.

## Watch out
- A new sink must implement its own fail-off (Deactivate -> off) plus a stall watchdog equivalent to local_argb::watch_progress(); LedActionSink relies on that for stalled providers.
- AGENTS.md, module-boundaries.md and firmware-composition.md call WeAct the sole firmware target; the OBD2 ESP32-S3 dongle record is proposed-only and forbids building vehicle firmware for it until reviewed.
