---
title: Keep hot notification traversal inside a stable function
author: Ke
date: 2026-10-27
branch: codex/gh-320-hot-placement
touches:
  - components/mazda_telemetry/src/vehicle_telemetry.cpp
  - docs/development/flash-cache-layout.md
  - firmware/weact-can485-v1.1/main/hot_code.lf
  - tests/tools/check_iram_placement_test.py
  - tools/check_iram_placement.py
---

## Decided
For #320, notification due/evaluation traversals use a private direct fold helper and per-descriptor visitor forced inline on ESP32 into the existing IRAM-pinned publish_notifications function. The linker fragment keeps the stable parent symbol and no complete tuple-wide lambda name. A checker-only fragment comment names the demangled parent; raw and demangled GNU nm --no-sort records are paired and address/type checked before discovering all parent-related local code.

## Tried and ruled out
Forcing only a std::apply lambda inline caused GCC 14 to outline a std::__invoke_impl helper into flash. Matching its raw mangling against the parent's mangling missed context-dependent ABI substitutions. An actual escaped helper is preserved as an independent checker regression vector. The direct fold removes the invoke layer; final production/profiling ELFs emit no parent-related lambda/visitor/invoke helper.

## Watch out
Keep exact raw linker-symbol validation and fail closed on nm failure, symbol-table misalignment, or a missing demangled parent. The host-only descriptor extension and both profiling flags exercise traversal accounting. Measure production and worst-case profiling resource usage before changing inline boundaries; no blanket object IRAM placement or hardware validation is implied. PR #303 remains separate.
