---
title: Host catalog regeneration is independent of firmware environment failures
author: Ke
date: 2026-10-27
branch: codex/gh-312-manifest
touches:
  - ".github/workflows/ci.yml"
  - CMakeLists.txt
  - docs/development/adding-a-signal.md
  - tools/controller_signal_catalog.json
  - tests/tools/generate_controller_signal_catalog_test.py
  - tools/generate_controller_signal_catalog.py
---

## Decided
The controller signal manifest is regenerated or checked through a standalone host exporter with BUILD_TESTING=OFF and MAZDA_BUILD_HOST_TESTS=OFF. Firmware configuration consumes the checked-in manifest and never builds or executes that host exporter. The explicit host regenerate/check targets are supported authoring operations.

## Tried and ruled out
A historical ESP-IDF bootloader configure failure was environment evidence from that session. It did not establish a prohibition on host builds, manifest regeneration, or later firmware validation. Do not interpret its lexical match as a reason to remove documented host commands.

## Watch out
The non-mutating check must leave stale or missing output untouched. Preserve independent manifest compatibility checks and keep target execution out of firmware configuration. Any actual firmware validation still needs the repository's supported SDK and separate current evidence.
