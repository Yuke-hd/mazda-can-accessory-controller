---
title: M8 post-306 boot repair acceptance
author: Codex
date: 2026-11-02
branch: codex/gh-401-final-acceptance
touches:
  - firmware/weact-can485-v1.1/main/main.cpp
  - components/companion_config/src/config_service.cpp
  - components/controller_config/src/persisted/application.cpp
  - tests/host/selected_profile_config_tests.cpp
  - docs/development/profile-runtime-validation.md
---

## Decided
- When persisted override or lighting startup fails, keep the companion repair path available. Report no active configuration while retaining the config store so a new valid document can be uploaded.
- A refused vehicle startup keeps CAN stopped and lighting black. Preserve listen-only CAN and a zero-length transmit queue whenever acquisition starts.
- If a merged task changes application startup code, refresh all eight actual firmware application images before reusing resource evidence. Record real selected-service storage and final ELF/table evidence.
- After a whitespace-only source update, prove the whitespace-insensitive diff is empty, then incrementally rebuild and size every existing image and record the final source SHA.
- Keep the full host build outside the source tree so the nested selected-profile matrix can create its own build directory.

## Tried and ruled out
- An outer CMake build directory inside the worktree caused the nested CTest matrix to reject its output path. This was a test-path issue, not a product defect.
- Use the actual service and linked ELF for resource claims. An early IDF expansion or retained-link probe does not measure the selected application's steady allocation.

## Watch out
- Host, tooling, and firmware CI results are software evidence. They do not establish bench or vehicle behavior.
- Keep the parent milestone open until its staged PR stack is merged; do not close it manually during acceptance.
