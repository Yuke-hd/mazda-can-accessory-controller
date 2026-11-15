---
title: "Round display link: CI runs tests/tools via unittest discover"
author: Ke
date: 2026-11-15
branch: claude/round-display-communication-e8a47b
cost: { minutes: 29, tokens: 204849 }
touches:
  - docs/SPEC.md
  - tasks/plan.md
  - firmware/weact-can485-v1.1/main/main.cpp
  - docs/README.md
  - docs/architecture/module-boundaries.md
  - docs/development/firmware-build.md
  - docs/specs/companion/display-espnow.md
  - tests/host/CMakeLists.txt
  - tests/tools/provision_display_code_test.py
  - tools/provision_display_code.py
---

## Decided
- In start_vehicle_io(), extra telemetry-started marks (display_espnow) go BEFORE companion_ble::mark_telemetry_started(); the local_argb boundary validator fixtures anchor on that call directly preceding `return true;`.
- Branch commits must be authored by the coding bot (re-author unpushed commits via the github-app-pr-flow wrapper with `commit --amend --reset-author`).

## Watch out
- CTest does not run tests/tools/*_test.py; CI's host job does via `python3 -m unittest discover -s tests/tools -p "*_test.py"`. Run that locally before pushing any main.cpp change.
- Auto-fix cannot be enabled on bot-authored PRs.
